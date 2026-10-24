param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [string]$IconPath
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

# Scarlet PC owns the software-keyboard frontend used by every swkbd applet call.
$keyboardSourceDir = Join-Path $repoRoot 'runtime\nswitch'
$keyboardTargetDir = Join-Path $SourceDir ('src\' + [string]$provenance.legacy.command_dir)
foreach ($name in @('scarlet_software_keyboard.h', 'scarlet_software_keyboard.cpp')) {
    $src = Join-Path $keyboardSourceDir $name
    $dst = Join-Path $keyboardTargetDir $name
    if (-not (Test-Path $src -PathType Leaf)) {
        throw "Missing Scarlet keyboard source: $src"
    }
    Copy-Item $src $dst -Force
}

# Project-owned FidelityFX adapter; the AMD SDK itself remains under third_party.
$graphicsSourceDir = Join-Path $repoRoot 'runtime\nswitch'
$graphicsTargetDir = Join-Path $SourceDir 'src\video_core\renderer_vulkan'
foreach ($name in @(
    'scarlet_ffx_framegen.h',
    'scarlet_ffx_framegen.cpp',
    'scarlet_dlss_client.h',
    'scarlet_dlss_client.cpp',
    'scarlet_dlss_pass.h',
    'scarlet_dlss_pass.cpp'
)) {
    $src = Join-Path $graphicsSourceDir $name
    $dst = Join-Path $graphicsTargetDir $name
    if (-not (Test-Path $src -PathType Leaf)) {
        throw "Missing Scarlet graphics source: $src"
    }
    Copy-Item $src $dst -Force
}

$dlssBridgeHeader = Join-Path $repoRoot 'runtime\nvidia\scarlet_dlss_bridge.h'
if (-not (Test-Path $dlssBridgeHeader -PathType Leaf)) {
    throw "Missing Scarlet DLSS bridge ABI header: $dlssBridgeHeader"
}
Copy-Item $dlssBridgeHeader (Join-Path $graphicsTargetDir 'scarlet_dlss_bridge.h') -Force

$ffxIncludeDir = Join-Path $repoRoot 'third_party\FidelityFX-SDK\ffx-api\include'
if (-not (Test-Path (Join-Path $ffxIncludeDir 'ffx_api\ffx_framegeneration.h') -PathType Leaf)) {
    throw "FidelityFX headers are missing. Run tools\scarlet_fetch_fidelityfx.ps1 first."
}
$ffxIncludeCmake = $ffxIncludeDir.Replace('\', '/')


function Expand-Legacy([string]$Text) {
    return $Text.Replace('__PRODUCT__', $legacyProduct).
        Replace('__PASCAL__', $legacyPascal).
        Replace('__UPPER__', $legacyUpper).
        Replace('__COMMAND__', $legacyCommandName)
}

function Replace-Literal([string]$RelativePath, [string]$Old, [string]$New) {
    $RelativePath = Expand-Legacy $RelativePath
    $displayPath = $RelativePath.Replace($legacyProduct, 'runtime').Replace($legacyPascal, 'Runtime')
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Scarlet runtime overlay: missing runtime source file $displayPath"
    }
    $Old = Expand-Legacy $Old
    $New = Expand-Legacy $New
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($Old)) {
        $text = $text.Replace($Old, $New)
        [IO.File]::WriteAllText($path, $text, $utf8)
        return
    }

    # Match multiline patches independently of Git/Windows LF vs CRLF conversion.
    $hadCrLf = $text.Contains("`r`n")
    $textLf = $text.Replace("`r`n", "`n")
    $oldLf = $Old.Replace("`r`n", "`n")
    $newLf = $New.Replace("`r`n", "`n")
    if ($textLf.Contains($oldLf)) {
        $textLf = $textLf.Replace($oldLf, $newLf)
        if ($hadCrLf) {
            $textLf = $textLf.Replace("`n", "`r`n")
        }
        [IO.File]::WriteAllText($path, $textLf, $utf8)
        return
    }

    if ($text.Contains($New) -or $textLf.Contains($newLf)) {
        return
    }

    $preview = $oldLf
    if ($preview.Length -gt 180) { $preview = $preview.Substring(0, 180) + '...' }
    $preview = $preview.Replace("`n", ' / ')
    throw "Scarlet runtime overlay: expected source text not found in $displayPath :: $preview"
}

function Replace-RegexOnce([string]$RelativePath, [string]$Pattern, [string]$Replacement) {
    $RelativePath = Expand-Legacy $RelativePath
    $displayPath = $RelativePath.Replace($legacyProduct, 'runtime').Replace($legacyPascal, 'Runtime')
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
        throw "Scarlet runtime overlay: expected pattern not found in $displayPath"
    }
}

# Build the Scarlet virtual-keyboard frontend into the command host.
$old = @'
        native_status.cpp
        native_status.h
        sdl_config.cpp
'@
$new = @'
        native_status.cpp
        native_status.h
        scarlet_software_keyboard.cpp
        scarlet_software_keyboard.h
        sdl_config.cpp
'@
Replace-Literal 'src\__PRODUCT___cmd\CMakeLists.txt' $old.Trim() $new.Trim()

# Compile Scarlet's FidelityFX bridge into the Vulkan video core.
$old = @'
    renderer_vulkan/vk_swapchain.cpp
    renderer_vulkan/vk_swapchain.h
    renderer_vulkan/vk_texture_cache.cpp
'@
$new = @'
    renderer_vulkan/vk_swapchain.cpp
    renderer_vulkan/vk_swapchain.h
    renderer_vulkan/scarlet_ffx_framegen.cpp
    renderer_vulkan/scarlet_ffx_framegen.h
    renderer_vulkan/scarlet_dlss_bridge.h
    renderer_vulkan/scarlet_dlss_client.cpp
    renderer_vulkan/scarlet_dlss_client.h
    renderer_vulkan/scarlet_dlss_pass.cpp
    renderer_vulkan/scarlet_dlss_pass.h
    renderer_vulkan/vk_texture_cache.cpp
'@
Replace-Literal 'src\video_core\CMakeLists.txt' $old.Trim() $new.Trim()

$old = 'target_include_directories(video_core PRIVATE ${HOST_SHADERS_INCLUDE})'
$new = $old + [Environment]::NewLine + 'target_include_directories(video_core PRIVATE "' + $ffxIncludeCmake + '")'
Replace-Literal 'src\video_core\CMakeLists.txt' $old $new


# Register the Scarlet software keyboard before Core initializes default applets.
$old = @'
#include "__PRODUCT___cmd/native_status.h"
'@
$new = @'
#include "__PRODUCT___cmd/native_status.h"
#include "__PRODUCT___cmd/scarlet_software_keyboard.h"
#include "core/hle/service/am/frontend/applets.h"
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
    Core::System system{};
    system.Initialize();
'@
$new = @'
    Core::System system{};
    Service::AM::Frontend::FrontendAppletSet scarlet_applets{};
    scarlet_applets.software_keyboard =
        std::make_unique<ScarletPC::SoftwareKeyboardApplet>();
    system.SetFrontendAppletSet(std::move(scarlet_applets));
    system.Initialize();
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

# Build the host directly with the Scarlet PC executable name.
$old = @'
set(__UPPER___CMD_TARGETS __COMMAND__)
'@
$new = @'
set(__UPPER___CMD_TARGETS __COMMAND__)
    set_target_properties(__COMMAND__ PROPERTIES OUTPUT_NAME "ScarletPC")
'@
Replace-Literal 'src\__PRODUCT___cmd\CMakeLists.txt' $old.Trim() $new.Trim()

# Player-facing Scarlet PC identity.
$old = @'
LOG_INFO(Frontend, "__COMMAND__ starting up...");
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.Runtime] Scarlet PC starting");
    const char* nswitch_fallback = std::getenv("NSWITCH_AOT_FALLBACK");
    if (!nswitch_fallback || *nswitch_fallback != '0') {
#ifdef _WIN32
        _putenv_s("__UPPER___RECOMP_STRICT", "0");
#else
        setenv("__UPPER___RECOMP_STRICT", "0", 0);
#endif
    }
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()
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

$pattern = 'LOG_INFO\(Frontend, "Native recompiled module \[\{\}\] loaded from \{\} [^"]*ArmRecomp active",\s*s_recomp_modules\.size\(\) - 1, Common::UTF16ToUTF8\(dll_name\)\);'
$replacement = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Module [{}] {} loaded",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
'@
Replace-RegexOnce 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $pattern $replacement.Trim()

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
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"__PRODUCT__ (Tools > Install Firmware), then start the game again.\n\nYou can "' '"Scarlet PC, then start the game again.\n\nYou can "'
$old = @'
"Install current keys in __PRODUCT__ (Tools > Install Decryption "
                                    "Keys), then start the game again. Copying prod.keys and "
'@
$new = @'
"Install current keys for Scarlet PC, then start the game again. "
                                    "Copying prod.keys and "
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()
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

# On Windows, ScarletPC.exe owns the window/taskbar icon. Always use PE icon resource 1
# instead of falling back to the third-party embedded BMP when native-export mode is off.
$pattern = 'void EmuWindow_SDL2::SetWindowIcon\(\) \{\s*#ifdef _WIN32\s*// Native game exports.*?if \(g_native_export_mode\) \{\s*const HICON hicon = static_cast<HICON>\(\s*LoadImageW\(GetModuleHandleW\(nullptr\), MAKEINTRESOURCEW\(1\), IMAGE_ICON, 256, 256,\s*LR_DEFAULTCOLOR\)\);'
$replacement = @'
void EmuWindow_SDL2::SetWindowIcon() {
#ifdef _WIN32
    {
        const HICON hicon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256,
                       LR_DEFAULTCOLOR));
'@
Replace-RegexOnce 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $pattern $replacement.Trim()

# Do not fall back to the embedded legacy BMP on Windows. If the PE resource cannot be
# loaded, leave the window icon unset rather than showing an unrelated product icon.
$old = @'
        LOG_WARNING(Frontend, "Native export: failed to load game icon from exe resources, "
                               "falling back to __PRODUCT__ icon.");
    }
#endif
    SDL_IOStream* const __PRODUCT___icon_stream = SDL_IOFromConstMem((void*)__PRODUCT___icon, __PRODUCT___icon_size);
'@
$new = @'
        LOG_WARNING(Frontend, "[Scarlet.Runtime] Failed to load Scarlet PC icon from executable resources.");
        return;
    }
#else
    SDL_IOStream* const __PRODUCT___icon_stream = SDL_IOFromConstMem((void*)__PRODUCT___icon, __PRODUCT___icon_size);
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

# Close the non-Windows fallback branch after the embedded BMP path.
$old = @'
    SDL_SetWindowIcon(render_window, window_icon);
    SDL_DestroySurface(window_icon);
}
'@
$new = @'
    SDL_SetWindowIcon(render_window, window_icon);
    SDL_DestroySurface(window_icon);
#endif
}
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

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

Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'const std::string title = __PASCAL__Cmd::FormatNativeTitle(display);' 'const std::string title = __PASCAL__Cmd::FormatNativeTitle(display);'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Preparing shaders...");' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet | Preparing shaders...");'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Building shaders %zu/%zu", built, total);' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet | Building shaders %zu/%zu", built, total);'

# Use the Scarlet icon in the Windows PE resource. Point the .rc file at a
# Scarlet-owned filename so the resource source itself changes and Ninja rebuilds it.
$projectIcon = if (-not [string]::IsNullOrWhiteSpace($IconPath)) {
    (Resolve-Path $IconPath).Path
} else {
    Join-Path $repoRoot 'docs\icon\icon.ico'
}
$runtimeIcon = Join-Path $SourceDir 'dist\scarlet.ico'
if (-not (Test-Path $projectIcon -PathType Leaf)) {
    throw "Scarlet icon not found: $projectIcon"
}
Copy-Item $projectIcon $runtimeIcon -Force

Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.rc' '"../../dist/__PRODUCT__.ico"' '"../../dist/scarlet.ico"'

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

# Scarlet PC performance profile. Keep emulation/game speed at 100%, but move
# presentation and shader compilation off the hot CPU path. The environment escape
# hatch is useful for A/B testing driver-specific regressions.
$old = @'
        g_native_export_mode = true;

        // Decode video on the CPU unless the user has chosen otherwise.
'@
$new = @'
        g_native_export_mode = true;

        // Scarlet PC performance defaults: keep GPU work and presentation asynchronous,
        // but compile shaders synchronously by default. Async shader compilation can skip
        // character-material draws until a pipeline is ready (visible in character setup).
        Settings::values.use_speed_limit.SetValue(true);
        Settings::values.speed_limit.SetValue(100);
        Settings::values.use_asynchronous_gpu_emulation.SetValue(true);
        Settings::values.async_presentation.SetValue(true);
        Settings::values.use_disk_shader_cache.SetValue(true);
        Settings::values.use_vulkan_driver_pipeline_cache.SetValue(true);
        Settings::values.sync_memory_operations.SetValue(false);
        Settings::values.frame_pacing_mode.SetValue(Settings::FramePacingMode::Target_Auto);

        const char* async_shader_env = std::getenv("SCARLET_ASYNC_SHADERS");
        const bool async_shaders =
            async_shader_env == nullptr || *async_shader_env == '\0' || *async_shader_env != '0';
        Settings::values.use_asynchronous_shaders.SetValue(async_shaders);

        LOG_INFO(Frontend,
                 "[Scarlet.Perf] Async GPU/presentation enabled; async shaders={}; game speed=100%",
                 async_shaders ? "on" : "off");

        // Decode video on the CPU unless the user has chosen otherwise.
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

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

# Avoid entering Dynarmic/JIT for common AArch64 memory instructions that an older
# AOT image can report as gaps. The first JIT creation forces every AOT module into
# verify-always mode, so handling these tiny instructions here has disproportionate value.
$old = @'
    }

    /// Base address of the module containing `pc`, so an address can be turned
'@
$new = @'
    }

    bool TryScarletFastGap(u32 opcode) {
        static const bool enabled = [] {
            const char* value = std::getenv("SCARLET_AOT_FAST_GAPS");
            return value == nullptr || *value == '\0' || *value != '0';
        }();
        if (!enabled) {
            return false;
        }

        // RET is by far Scarlet's hottest AOT gap. Returning through Dynarmic for
        // every lifted function destroys the benefit of static recompilation.
        // RET Xn simply transfers control to Xn without touching flags.
        if ((opcode & 0xFFFFFC1FU) == 0xD65F0000U) {
            const u32 rn = (opcode >> 5) & 31U;
            if (rn == 31U) {
                return false;
            }
            ctx.pc = ctx.x[rn];
            return true;
        }

        // ADD/SUB (immediate), without flag updates. This covers the extremely hot
        // stack-pointer adjustments emitted in function prologues/epilogues such as
        // ADD SP,SP,#imm and SUB SP,SP,#imm.
        if ((opcode & 0x1F000000U) == 0x11000000U &&
            (opcode & 0x20000000U) == 0) {
            const bool is64 = (opcode & 0x80000000U) != 0;
            const bool subtract = (opcode & 0x40000000U) != 0;
            const u32 rn = (opcode >> 5) & 31U;
            const u32 rd = opcode & 31U;
            const u64 imm12 = (opcode >> 10) & 0xFFFU;
            const u64 immediate = imm12 << (((opcode >> 22) & 1U) ? 12U : 0U);
            const u64 lhs = is64 ? ctx.x[rn] : static_cast<u32>(ctx.x[rn]);
            const u64 raw = subtract ? lhs - immediate : lhs + immediate;
            const u64 result = is64 ? raw : static_cast<u32>(raw);
            ctx.x[rd] = result;
            ctx.pc += 4;
            return true;
        }

        // MOV register aliases (ORR W/Xd, W/XZR, W/Xm with LSL #0).
        if ((opcode & 0x7FE0FFE0U) == 0x2A0003E0U) {
            const bool is64 = (opcode & 0x80000000U) != 0;
            const u32 rm = (opcode >> 16) & 31U;
            const u32 rd = opcode & 31U;
            if (rd != 31U) {
                const u64 source = rm == 31U ? 0 : ctx.x[rm];
                ctx.x[rd] = is64 ? source : static_cast<u32>(source);
            }
            ctx.pc += 4;
            return true;
        }

        // MOVZ W/Xd,#imm{,LSL #16/#32/#48}.
        if ((opcode & 0x7F800000U) == 0x52800000U) {
            const bool is64 = (opcode & 0x80000000U) != 0;
            const u32 hw = (opcode >> 21) & 3U;
            if (!is64 && hw > 1U) {
                return false;
            }
            const u32 rd = opcode & 31U;
            const u64 imm16 = (opcode >> 5) & 0xFFFFU;
            const u64 value = imm16 << (hw * 16U);
            if (rd != 31U) {
                ctx.x[rd] = is64 ? value : static_cast<u32>(value);
            }
            ctx.pc += 4;
            return true;
        }

        // ADRP Xd,label. Keep PC-relative address formation inside AOT rather than
        // crossing into JIT for a single constant-page calculation.
        if ((opcode & 0x9F000000U) == 0x90000000U) {
            const u32 rd = opcode & 31U;
            s64 imm21 = static_cast<s64>(((opcode >> 29) & 3U) |
                                         (((opcode >> 5) & 0x7FFFFU) << 2));
            if ((imm21 & (1LL << 20)) != 0) {
                imm21 -= (1LL << 21);
            }
            if (rd != 31U) {
                const u64 page = ctx.pc & ~0xFFFULL;
                ctx.x[rd] = page + static_cast<u64>(imm21 * 4096LL);
            }
            ctx.pc += 4;
            return true;
        }

        // Integer LDR/STR (unsigned immediate), byte/half/word/dword.
        // These are extremely common in SDK/runtime code and are safe to execute
        // directly through the same memory callbacks used by generated AOT code.
        const u32 top = opcode & 0xFFC00000U;
        u32 width = 0;
        bool load = false;
        switch (top) {
        case 0x39000000U: width = 1; load = false; break; // STRB
        case 0x39400000U: width = 1; load = true;  break; // LDRB
        case 0x79000000U: width = 2; load = false; break; // STRH
        case 0x79400000U: width = 2; load = true;  break; // LDRH
        case 0xB9000000U: width = 4; load = false; break; // STR Wt
        case 0xB9400000U: width = 4; load = true;  break; // LDR Wt
        case 0xF9000000U: width = 8; load = false; break; // STR Xt
        case 0xF9400000U: width = 8; load = true;  break; // LDR Xt
        default: break;
        }
        if (width != 0) {
            const u32 rt = opcode & 31U;
            const u32 rn = (opcode >> 5) & 31U;
            const u64 imm12 = (opcode >> 10) & 0xFFFU;
            const u64 address = ctx.x[rn] + imm12 * width;
            if (load) {
                const u64 value = HostLoad(this, address, width);
                if (rt != 31U) {
                    ctx.x[rt] = width == 8 ? value : static_cast<u32>(value);
                }
            } else {
                const u64 value = rt == 31U ? 0 : ctx.x[rt];
                HostStore(this, address, width, value);
            }
            ctx.pc += 4;
            return true;
        }

        // Integer STP/LDP, including offset, pre-index, post-index and non-temporal
        // forms. Restrict opc to ordinary W/X pairs (not LDPSW or SIMD pairs).
        if ((opcode & 0x7E000000U) == 0x28000000U) {
            const u32 rt = opcode & 31U;
            const u32 rn = (opcode >> 5) & 31U;
            const u32 rt2 = (opcode >> 10) & 31U;
            const u32 mode = (opcode >> 23) & 3U;
            const bool pair_load = ((opcode >> 22) & 1U) != 0;
            const u32 pair_width = (opcode & 0x80000000U) != 0 ? 8U : 4U;

            // Load-pair writeback overlapping its base is constrained-unpredictable;
            // leave rare cases to the full fallback rather than guessing.
            const bool writeback = mode == 1U || mode == 3U;
            if (pair_load && writeback && rn != 31U && (rn == rt || rn == rt2)) {
                return false;
            }

            const s64 raw_imm7 = static_cast<s64>((opcode >> 15) & 0x7FU);
            const s64 signed_imm7 = raw_imm7 >= 64 ? raw_imm7 - 128 : raw_imm7;
            const s64 byte_offset = signed_imm7 * static_cast<s64>(pair_width);
            const u64 base = ctx.x[rn];
            const u64 adjusted = base + static_cast<u64>(byte_offset);
            const u64 address = mode == 1U ? base : adjusted;

            if (pair_load) {
                const u64 first = HostLoad(this, address, pair_width);
                const u64 second = HostLoad(this, address + pair_width, pair_width);
                if (rt != 31U) {
                    ctx.x[rt] = pair_width == 8 ? first : static_cast<u32>(first);
                }
                if (rt2 != 31U) {
                    ctx.x[rt2] = pair_width == 8 ? second : static_cast<u32>(second);
                }
            } else {
                const u64 first = rt == 31U ? 0 : ctx.x[rt];
                const u64 second = rt2 == 31U ? 0 : ctx.x[rt2];
                HostStore(this, address, pair_width, first);
                HostStore(this, address + pair_width, pair_width, second);
            }

            if (writeback) {
                ctx.x[rn] = adjusted;
            }
            ctx.pc += 4;
            return true;
        }

        return false;
    }

    /// Base address of the module containing `pc`, so an address can be turned
'@
Replace-Literal 'src\core\arm\recomp\arm_recomp.cpp' $old.Trim() $new.Trim()

$old = @'
            g_counters.fallback_from_unhandled.fetch_add(1, std::memory_order_relaxed);
            const u32 unsupported_opcode =
                static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4));
            g_counters.RecordUnhandled(unsupported_opcode);
            RecompGaps::RecordUnimplemented(unsupported_opcode);
'@
$new = @'
            const u32 unsupported_opcode =
                static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4));

            // Keep common load/store gaps inside the AOT backend. Besides avoiding
            // a JIT transition, this prevents the first simple gap from pinning all
            // four native modules to per-entry verification.
            if (impl->TryScarletFastGap(unsupported_opcode)) {
                static std::atomic<int> fast_gap_logs{0};
                if (fast_gap_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOG_INFO(Core_ARM, "[Scarlet.AOT] Fast-gap opcode {:#010x} executed at {:#x}",
                             unsupported_opcode, impl->ctx.pc - 4);
                }
                continue;
            }

            g_counters.fallback_from_unhandled.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordUnhandled(unsupported_opcode);
            RecompGaps::RecordUnimplemented(unsupported_opcode);
'@
Replace-Literal 'src\core\arm\recomp\arm_recomp.cpp' $old.Trim() $new.Trim()

# Keep guarded AOT modules on generation checks even when a rare instruction still
# needs Dynarmic. A fallback JIT normally uses fastmem/page-table direct accesses,
# which bypass the recompiler watch hooks and therefore forces every AOT module to
# verify every instruction on entry forever. For the AOT fallback only, force memory
# through Core::Memory callbacks; those already call OnWatchedWrite/WriteExclusive.
$old = @'
    ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process,
                  DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index);
'@
$new = @'
    ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process,
                  DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index,
                  bool recomp_fallback = false);
'@
Replace-Literal 'src\core\arm\dynarmic\arm_dynarmic_64.h' $old.Trim() $new.Trim()

$old = @'
    std::optional<DynarmicCallbacks64> m_cb{};
    std::size_t m_core_index{};

    std::optional<Dynarmic::A64::Jit> m_jit{};
'@
$new = @'
    std::optional<DynarmicCallbacks64> m_cb{};
    std::size_t m_core_index{};
    bool m_recomp_fallback{};

    std::optional<Dynarmic::A64::Jit> m_jit{};
'@
Replace-Literal 'src\core\arm\dynarmic\arm_dynarmic_64.h' $old.Trim() $new.Trim()

$old = @'
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    m_jit.emplace(config);
'@
$new = @'
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }

    if (m_recomp_fallback) {
        // Guarded AOT code must observe every fallback write. Direct page-table and
        // fastmem paths bypass Core::Memory::NoteRecompWrite, while callbacks do not.
        config.page_table = nullptr;
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
        config.recompile_on_exclusive_fastmem_failure = false;
    }

    m_jit.emplace(config);
'@
Replace-Literal 'src\core\arm\dynarmic\arm_dynarmic_64.cpp' $old.Trim() $new.Trim()

$old = @'
ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}
'@
$new = @'
ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index, bool recomp_fallback)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}, m_recomp_fallback{recomp_fallback}
'@
Replace-Literal 'src\core\arm\dynarmic\arm_dynarmic_64.cpp' $old.Trim() $new.Trim()

$old = @'
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process,
            static_cast<DynarmicExclusiveMonitor&>(*impl->exclusive_monitor), impl->core_index);
'@
$new = @'
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process,
            static_cast<DynarmicExclusiveMonitor&>(*impl->exclusive_monitor), impl->core_index,
            true);
'@
Replace-Literal 'src\core\arm\recomp\arm_recomp.cpp' $old.Trim() $new.Trim()

$old = @'
void OnJitFallback() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    StickyWhere(s, Reason::JitFallback, PinCause::JitFallback,
                [](const ModuleState&, std::uint64_t&) { return true; });
}
'@
$new = @'
void OnJitFallback() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    // Scarlet's AOT fallback is constructed without fastmem/page-table direct
    // memory, so every JIT write goes through Core::Memory and its watch hooks.
    // Advance the generation once for the engine transition, but do not pin all
    // modules to verify-always for the rest of the process.
    Bump(s, Reason::JitFallback);
}
'@
Replace-Literal 'src\core\arm\recomp\recomp_guard_gen.cpp' $old.Trim() $new.Trim()

# Scarlet-owned default local profile identity.
Replace-Literal 'src\core\hle\service\acc\profile_manager.cpp' 'CreateNewUser(UUID::MakeRandom(), "__PRODUCT__");' 'CreateNewUser(UUID::MakeRandom(), "Scarlet");'
Replace-Literal 'src\core\hle\service\acc\profile_manager.cpp' '"user ''__PRODUCT__'' with random UUID.");' '"user ''Scarlet'' with random UUID.");'
Replace-Literal 'src\core\hle\service\acc\profile_manager.cpp' '"''__PRODUCT__'' with random UUID.");' '"''Scarlet'' with random UUID.");'

# Keep the generic ROM-load failure entirely Scarlet-facing.
$old = @'
LOG_CRITICAL(Frontend,
                         "While attempting to load the ROM requested, an error occurred. Please "
                         "refer to the __PRODUCT__ wiki for more information or the __PRODUCT__ discord for "
                         "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
'@
$new = @'
LOG_CRITICAL(Frontend,
                         "[Scarlet.Game] The game could not be loaded. Check the Scarlet PC log for details. "
                         "Error Code: {:04X}-{:04X}; Error Description: {}",
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

# Scarlet PC owns its player-facing log filename. Keep upstream path/layout
# internals intact, but never emit the inherited product log filename.
Replace-Literal 'src\common\logging\backend.cpp' 'new Impl(log_dir / LOG_FILE, filter)' 'new Impl(log_dir / "scarlet_log.txt", filter)'
Replace-Literal 'src\common\logging.cpp' 'logging_instance->file_backend.emplace(log_dir / LOG_FILE);' 'logging_instance->file_backend.emplace(log_dir / "scarlet_log.txt");'
$old = @'
        auto old_filename = filename;
        old_filename += ".old.txt";
'@
$new = @'
        const auto old_filename = filename.parent_path() / "scarlet_log.old.txt";
'@
Replace-Literal 'src\common\logging\backend.cpp' $old.Trim() $new.Trim()
Replace-Literal 'src\common\logging.cpp' $old.Trim() $new.Trim()

# Sanitize legacy command-host source paths at the common logging choke point.
# Both logger frontends eventually call FmtLogMessage(), so this works even if a
# legacy formatter object is reused by the build. Keep useful source names, but
# never expose the inherited command directory/product name in player-facing logs.
Replace-Literal 'src\common\logging.h' '#include <type_traits>' "#include <type_traits>`n#include <string_view>"
$old = @'
template <typename... Args>
void FmtLogMessage(Class log_class, Level log_level, const char* filename, unsigned int line_num, const char* function, fmt::format_string<Args...> format, const Args&... args) {
    FmtLogMessageImpl(log_class, log_level, filename, line_num, function, format.get(), fmt::make_format_args(args...));
}
'@
$new = @'
inline const char* ScarletLogSource(const char* filename) {
    if (filename == nullptr) {
        return "scarlet_runtime.cpp";
    }

    const std::string_view source{filename};
    constexpr std::string_view legacy_backslash{"suyu_cmd\\"};
    constexpr std::string_view legacy_slash{"suyu_cmd/"};

    std::size_t pos = source.rfind(legacy_backslash);
    std::size_t prefix_size = legacy_backslash.size();
    if (pos == std::string_view::npos) {
        pos = source.rfind(legacy_slash);
        prefix_size = legacy_slash.size();
    }

    if (pos != std::string_view::npos) {
        const char* trimmed = filename + pos + prefix_size;
        if (std::string_view{trimmed} == "suyu.cpp") {
            return "scarlet_runtime.cpp";
        }
        return trimmed;
    }

    return filename;
}

template <typename... Args>
void FmtLogMessage(Class log_class, Level log_level, const char* filename, unsigned int line_num, const char* function, fmt::format_string<Args...> format, const Args&... args) {
    FmtLogMessageImpl(log_class, log_level, ScarletLogSource(filename), line_num, function,
                      format.get(), fmt::make_format_args(args...));
}
'@
Replace-Literal 'src\common\logging.h' $old.Trim() $new.Trim()

# Scarlet runtime logs keep only time/category/level/message. This intentionally
# removes source file, function and line metadata so legacy internal source paths never leak.
$old = @'
return fmt::format("[{:4d}.{:06d}] {} <{}> {}:{}:{}: {}", time_seconds, time_fractional,
                       class_name, level_name, entry.filename, entry.function, entry.line_num,
                       entry.message);
'@
$new = @'
return fmt::format("[{:4d}.{:06d}] {} <{}> {}", time_seconds, time_fractional,
                       class_name, level_name, entry.message);
'@
Replace-Literal 'src\common\logging\text_formatter.cpp' $old.Trim() $new.Trim()

# The validated runtime revision contains a second/legacy logger in common/logging.cpp.
# Patch it too; otherwise __FILE__ still exposes legacy command-directory/product names
# such as "suyu_cmd\suyu.cpp" even though the newer formatter above is Scarlet-facing.
$old = @'
return fmt::format("[{:4d}.{:06d}] {} <{}> {}:{}:{}: {}", time_seconds, time_fractional, class_name, level_name, entry.filename, entry.line_num, entry.function, entry.message);
'@
$new = @'
return fmt::format("[{:4d}.{:06d}] {} <{}> {}", time_seconds, time_fractional,
                       class_name, level_name, entry.message);
'@
Replace-Literal 'src\common\logging.cpp' $old.Trim() $new.Trim()

# Scarlet-facing live title: FPS, frametime, shader activity and fallback transitions.
# Keep an accumulated compiled-shader counter for the Scarlet title bar.
$old = @'
    [[nodiscard]] int ShadersBuilding() noexcept;

    void MarkShaderComplete() noexcept {
'@
$new = @'
    [[nodiscard]] int ShadersBuilding() noexcept;
    [[nodiscard]] int ShadersCompiled() const noexcept {
        return num_complete.load(std::memory_order_relaxed);
    }

    void MarkShaderComplete() noexcept {
'@
Replace-Literal 'src\video_core\shader_notify.h' $old.Trim() $new.Trim()

$old = @'
    int shaders_building{};
'@
$new = @'
    int shaders_building{};
    int shaders_compiled{};
'@
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' $old.Trim() $new.Trim()

$old = @'
    if (display.game_running) {
        display.shaders_building = system.GPU().ShaderNotify().ShadersBuilding();
    }
'@
$new = @'
    if (display.game_running) {
        display.shaders_building = system.GPU().ShaderNotify().ShadersBuilding();
        display.shaders_compiled = system.GPU().ShaderNotify().ShadersCompiled();
    }
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

$old = @'
    // Shown only while it is happening, so a stutter can be matched to it.
    if (s.shaders_building > 0) {
        out += " | Building " + std::to_string(s.shaders_building) +
               (s.shaders_building == 1 ? " shader" : " shaders");
    }
'@
$new = @'
    out += " | Shaders: " + std::to_string(s.shaders_compiled);
    if (s.shaders_building > 0) {
        out += " (building " + std::to_string(s.shaders_building) + ")";
    }
'@
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' $old.Trim() $new.Trim()

$old = @'
    out += " | ";
    out += NativeBackendName(ClassifyGameBackend(s));
    out += GameBackendQualifier(s);

'@
$new = @'

'@
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' $old $new

Replace-Literal 'src\__PRODUCT___cmd\native_status.h' 'return "suyu static AOT";' 'return "AOT";'
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' 'return "suyu Hybrid JIT + AOT";' 'return "AOT + dynamic fallback";'
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' 'return "suyu Dynarmic JIT";' 'return "Dynamic fallback";'
Replace-Literal 'src\__PRODUCT___cmd\native_status.h' 'out += " | F12 Controls";' 'out += " | F12 Controls";'

# Scarlet PC presentation target. The guest commonly requests swap interval 2
# (30 Hz) on Switch 1. For a 60+ target, normalize that request to one VI period
# at the HLE compositor boundary, preserving the AOT guest text byte-for-byte.
Replace-Literal 'src\core\hle\service\nvnflinger\hardware_composer.cpp' '#include <optional>' "#include <optional>`n#include <cstdlib>"

$old = @'
s32 NormalizeSwapInterval(f32* out_speed_scale, s32 swap_interval) {
    if (swap_interval <= 0) {
        // As an extension, treat nonpositive swap interval as speed multiplier.
        if (out_speed_scale) {
            *out_speed_scale = 2.f * static_cast<f32>(1 - swap_interval);
        }

        swap_interval = 1;
    }

    if (swap_interval >= 5) {
        // As an extension, treat high swap interval as precise speed control.
        if (out_speed_scale) {
            *out_speed_scale = static_cast<f32>(swap_interval) / 100.f;
        }

        swap_interval = 1;
    }

    return swap_interval;
}
'@
$new = @'
s32 NormalizeSwapInterval(f32* out_speed_scale, s32 swap_interval) {
    if (swap_interval <= 0) {
        if (out_speed_scale) {
            *out_speed_scale = 2.f * static_cast<f32>(1 - swap_interval);
        }
        swap_interval = 1;
    }

    if (swap_interval >= 5) {
        if (out_speed_scale) {
            *out_speed_scale = static_cast<f32>(swap_interval) / 100.f;
        }
        swap_interval = 1;
    }

    const char* target_env = std::getenv("SCARLET_TARGET_FPS");
    const int target_fps = target_env && *target_env ? std::atoi(target_env) : 60;
    if (target_fps >= 60 && swap_interval == 2) {
        swap_interval = 1;
    }

    return swap_interval;
}
'@
Replace-Literal 'src\core\hle\service\nvnflinger\hardware_composer.cpp' $old.Trim() $new.Trim()

# Make VI pacing follow the requested Scarlet target. 60 is the supported default;
# higher rates are experimental and may expose frame-dependent animations.
$old = @'
#include "common/settings.h"
'@
$new = @'
#include <algorithm>
#include <cstdlib>

#include "common/settings.h"
'@
Replace-Literal 'src\core\hle\service\vi\conductor.cpp' $old.Trim() $new.Trim()

$old = @'
constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};
'@
$new = @'
namespace {

int ScarletTargetFps() {
    static const int target = [] {
        const char* value = std::getenv("SCARLET_TARGET_FPS");
        int fps = value && *value ? std::atoi(value) : 60;
        return std::clamp(fps, 30, 240);
    }();
    return target;
}

std::chrono::nanoseconds ScarletFrameNs() {
    return std::chrono::nanoseconds{1000000000 / ScarletTargetFps()};
}

} // namespace
'@
Replace-Literal 'src\core\hle\service\vi\conductor.cpp' $old.Trim() $new.Trim()
Replace-Literal 'src\core\hle\service\vi\conductor.cpp' 'system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);' 'system.CoreTiming().ScheduleLoopingEvent(ScarletFrameNs(), ScarletFrameNs(), m_event);'

$old = @'
    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);
'@
$new = @'
    const f32 effective_fps =
        static_cast<f32>(ScarletTargetFps()) / static_cast<f32>(m_swap_interval);
'@
Replace-Literal 'src\core\hle\service\vi\conductor.cpp' $old.Trim() $new.Trim()


# Scarlet Launcher: external DLC files can be supplied repeatedly alongside the explicit base/update pair.
$old = @'
                  "--content-update      Read-only matching update NSP (both flags required)\n"
                  "--content-probe       Verify that pair and exit without a window or game\n"
'@
$new = @'
                  "--content-update      Read-only matching update NSP (both flags required)\n"
                  "--content-dlc         Read-only matching DLC NSP/XCI (repeatable)\n"
                  "--content-probe       Verify that pair and exit without a window or game\n"
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
    std::optional<std::string> explicit_content_base;
    std::optional<std::string> explicit_content_update;
    bool explicit_content_probe = false;
'@
$new = @'
    std::optional<std::string> explicit_content_base;
    std::optional<std::string> explicit_content_update;
    std::vector<std::string> explicit_content_dlc;
    bool explicit_content_probe = false;
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
        {"content-base", required_argument, 0, 'B'},
        {"content-update", required_argument, 0, 'U'},
        {"content-probe", no_argument, 0, 'P'},
'@
$new = @'
        {"content-base", required_argument, 0, 'B'},
        {"content-update", required_argument, 0, 'U'},
        {"content-dlc", required_argument, 0, 'A'},
        {"content-probe", no_argument, 0, 'P'},
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"g:fhvp::c:u:l::tV:N:B:U:PD:"' '"g:fhvp::c:u:l::tV:N:B:U:A:PD:"'

$old = @'
            case 'U':
                if (explicit_content_update) {
                    LOG_ERROR(Frontend, "Duplicate --content-update argument");
                    return 2;
                }
                explicit_content_update = optarg;
                break;
            case 'D':
'@
$new = @'
            case 'U':
                if (explicit_content_update) {
                    LOG_ERROR(Frontend, "Duplicate --content-update argument");
                    return 2;
                }
                explicit_content_update = optarg;
                break;
            case 'A':
                if (!optarg || !*optarg) {
                    LOG_ERROR(Frontend, "Invalid --content-dlc argument");
                    return 2;
                }
                explicit_content_dlc.emplace_back(optarg);
                break;
            case 'D':
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
    if (explicit_content_base && !filepath.empty()) {
        LOG_ERROR(Frontend, "--content-base owns the launch path; do not also supply -g or a positional game");
        return 2;
    }
'@
$new = @'
    if (!explicit_content_dlc.empty() && !explicit_content_base) {
        LOG_ERROR(Frontend, "--content-dlc requires --content-base and --content-update");
        return 2;
    }
    if (explicit_content_base && !filepath.empty()) {
        LOG_ERROR(Frontend, "--content-base owns the launch path; do not also supply -g or a positional game");
        return 2;
    }
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
#include <string>

#include "common/hex_util.h"
'@
$new = @'
#include <string>
#include <vector>

#include "common/hex_util.h"
'@
Replace-Literal 'src\__PRODUCT___cmd\explicit_update.h' $old.Trim() $new.Trim()

$old = @'
    return true;
}

} // namespace SuyuCli
'@
$new = @'
    return true;
}

inline bool AddExplicitDlc(Core::System& system, ExplicitUpdateProvider& provider,
                           const std::string& base_path, const std::string& dlc_path) {
    using FileSys::TitleType;

    auto& vfs = *system.GetFilesystem();
    const auto base_file = vfs.OpenFile(base_path, FileSys::OpenMode::Read);
    const auto dlc_file = vfs.OpenFile(dlc_path, FileSys::OpenMode::Read);
    if (!base_file || !dlc_file) {
        LOG_ERROR(Frontend, "CLI explicit DLC rejected: base or DLC could not be opened: {}",
                  dlc_path);
        return false;
    }

    const auto base_loader = Loader::GetLoader(system, base_file);
    u64 base_title_id{};
    if (!base_loader ||
        base_loader->ReadProgramId(base_title_id) != Loader::ResultStatus::Success ||
        base_title_id == 0) {
        LOG_ERROR(Frontend, "CLI explicit DLC rejected: base title ID could not be read");
        return false;
    }
    base_title_id = FileSys::GetBaseTitleID(base_title_id);

    FileSys::ManualContentProvider dlc_entries;
    if (!dlc_entries.AddEntriesFromContainer(dlc_file, false)) {
        LOG_ERROR(Frontend, "CLI explicit DLC rejected: container could not be read: {}",
                  dlc_path);
        return false;
    }

    const auto entries = dlc_entries.ListEntriesFilter(TitleType::AOC, {}, {});
    if (entries.empty()) {
        LOG_ERROR(Frontend, "CLI explicit DLC rejected: no Add-On Content records: {}",
                  dlc_path);
        return false;
    }

    std::size_t added = 0;
    for (const auto& entry : entries) {
        if (FileSys::GetBaseTitleID(entry.title_id) != base_title_id) {
            LOG_ERROR(Frontend,
                      "CLI explicit DLC rejected: title {:016X} belongs to another game",
                      entry.title_id);
            return false;
        }
        const auto file = dlc_entries.GetEntryRaw(entry.title_id, entry.type);
        if (!file) {
            LOG_ERROR(Frontend, "CLI explicit DLC rejected: content record could not be opened");
            return false;
        }
        provider.AddEntry(TitleType::AOC, entry.type, entry.title_id, file);
        ++added;
    }

    LOG_INFO(Frontend, "[Scarlet.Game] Registered {} external DLC content entries from {}",
             added, dlc_path);
    return true;
}

} // namespace SuyuCli
'@
Replace-Literal 'src\__PRODUCT___cmd\explicit_update.h' $old.Trim() $new.Trim()

$old = @'
        if (!SuyuCli::ConfigureExplicitUpdate(system, explicit_provider,
                                              *explicit_content_base, *explicit_content_update,
                                              explicit_content_dump)) {
            return 2;
        }
        if (explicit_content_probe) {
'@
$new = @'
        if (!SuyuCli::ConfigureExplicitUpdate(system, explicit_provider,
                                              *explicit_content_base, *explicit_content_update,
                                              explicit_content_dump)) {
            return 2;
        }
        for (const auto& dlc_path : explicit_content_dlc) {
            if (!SuyuCli::AddExplicitDlc(system, explicit_provider, *explicit_content_base,
                                         dlc_path)) {
                return 2;
            }
        }
        if (explicit_content_probe) {
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()





$old = @'
#include <optional>
#include <variant>
'@
$new = @'
#include <memory>
#include <optional>
#include <variant>
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.h' $old.Trim() $new.Trim()

# Route the original Scarlet display layer through NGX before the normal window-adaptation
# pass. This is a real DLSS SR evaluation; neutral depth/motion are the compatibility baseline.
$old = @'
#include "video_core/renderer_vulkan/present/smaa.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
'@
$new = @'
#include "video_core/renderer_vulkan/present/smaa.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/scarlet_dlss_client.h"
#include "video_core/renderer_vulkan/scarlet_dlss_pass.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()

$old = @'
class RasterizerVulkan;
class Scheduler;

class Layer final {
'@
$new = @'
class RasterizerVulkan;
class Scheduler;
class ScarletDlssPass;

class Layer final {
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.h' $old.Trim() $new.Trim()

$old = @'
    Settings::AntiAliasing anti_alias_setting{};
    std::variant<std::monostate, FXAA, SMAA> anti_alias{};
    std::variant<std::monostate, SGSR, FSR> sr_filter{};
'@
$new = @'
    Settings::AntiAliasing anti_alias_setting{};
    std::variant<std::monostate, FXAA, SMAA> anti_alias{};
    std::unique_ptr<ScarletDlssPass> dlss_filter{};
    std::variant<std::monostate, SGSR, FSR> sr_filter{};
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.h' $old.Trim() $new.Trim()

$old = @'
    if (filters.get_scaling_filter() == Settings::ScalingFilter::Fsr) {
        sr_filter.emplace<FSR>(device, memory_allocator, image_count, output_size);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::Sgsr) {
'@
$new = @'
    if (ScarletPC::Graphics::DlssClient::Get().Requested() &&
        ScarletPC::Graphics::DlssClient::Get().IsInitialized()) {
        dlss_filter =
            std::make_unique<ScarletDlssPass>(device, memory_allocator, image_count, output_size);
    }
    if (filters.get_scaling_filter() == Settings::ScalingFilter::Fsr) {
        sr_filter.emplace<FSR>(device, memory_allocator, image_count, output_size);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::Sgsr) {
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()

$old = @'
    auto crop_rect = Tegra::NormalizeCrop(framebuffer, texture_width, texture_height);
    const VkExtent2D render_extent{
        .width = scaled_width,
        .height = scaled_height,
    };

    if (auto* fsr = std::get_if<FSR>(&sr_filter)) {
'@
$new = @'
    auto crop_rect = Tegra::NormalizeCrop(framebuffer, texture_width, texture_height);
    const VkExtent2D render_extent{
        .width = scaled_width,
        .height = scaled_height,
    };

    if (dlss_filter && dlss_filter->Ready()) {
        const VkImageView dlss_view =
            dlss_filter->Draw(device, scheduler, image_index, source_image, source_image_view,
                              GetFormat(framebuffer), render_extent);
        if (dlss_view != source_image_view) {
            source_image_view = dlss_view;
            crop_rect = {0, 0, 1, 1};
        }
    }

    if (auto* fsr = std::get_if<FSR>(&sr_filter)) {
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()


# Optional NVIDIA NGX/DLSS integration. The bridge supplies extension requirements before
# Vulkan instance/device creation and is inert unless SCARLET_UPSCALER=dlss.
$old = @'
#include "core/frontend/emu_window.h"
#include "video_core/vulkan_common/vulkan_instance.h"
'@
$new = @'
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/scarlet_dlss_client.h"
#include "video_core/vulkan_common/vulkan_instance.h"
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_instance.cpp' $old.Trim() $new.Trim()

$old = @'
    if (enable_validation && AreExtensionsSupported(dld, properties, std::array{VK_EXT_DEBUG_UTILS_EXTENSION_NAME}))
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    return extensions;
'@
$new = @'
    if (enable_validation && AreExtensionsSupported(dld, properties, std::array{VK_EXT_DEBUG_UTILS_EXTENSION_NAME}))
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    if (auto& dlss = ScarletPC::Graphics::DlssClient::Get(); dlss.Requested()) {
        for (const auto& required : dlss.InstanceExtensions()) {
            const auto available = std::ranges::find_if(properties, [&](const auto& property) {
                return std::strcmp(required.extensionName, property.extensionName) == 0;
            });
            if (available == properties.end()) {
                LOG_WARNING(Render_Vulkan,
                            "[Scarlet.DLSS] Required Vulkan instance extension unavailable: {}",
                            required.extensionName);
                continue;
            }
            const auto duplicate = std::ranges::find_if(extensions, [&](const char* name) {
                return std::strcmp(name, required.extensionName) == 0;
            });
            if (duplicate == extensions.end()) {
                extensions.push_back(required.extensionName);
            }
        }
    }
    return extensions;
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_instance.cpp' $old.Trim() $new.Trim()

$old = @'
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
'@
$new = @'
#include "video_core/renderer_vulkan/scarlet_dlss_client.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.cpp' $old.Trim() $new.Trim()

$old = @'
    LOG_INFO(Render_Vulkan, "Graphics pipeline libraries: {}",
             IsGraphicsPipelineLibrarySupported() ? "enabled" : "unavailable or disabled");

    logical = vk::Device::Create(physical, queue_cis, ExtensionListForVulkan(loaded_extensions), first_next, dld);

    graphics_queue = logical.GetQueue(graphics_family);
'@
$new = @'
    LOG_INFO(Render_Vulkan, "Graphics pipeline libraries: {}",
             IsGraphicsPipelineLibrarySupported() ? "enabled" : "unavailable or disabled");

    if (auto& dlss = ScarletPC::Graphics::DlssClient::Get(); dlss.Requested()) {
        for (const auto& required : dlss.DeviceExtensions(instance, physical)) {
            if (!supported_extensions.contains(required.extensionName)) {
                LOG_WARNING(Render_Vulkan,
                            "[Scarlet.DLSS] Required Vulkan device extension unavailable: {}",
                            required.extensionName);
                continue;
            }
            loaded_extensions.insert(required.extensionName);
        }
    }

    logical = vk::Device::Create(physical, queue_cis, ExtensionListForVulkan(loaded_extensions), first_next, dld);

    if (auto& dlss = ScarletPC::Graphics::DlssClient::Get(); dlss.Requested()) {
        (void)dlss.Initialize(instance, physical, *logical, dld.vkGetInstanceProcAddr,
                              dld.vkGetDeviceProcAddr);
    }

    graphics_queue = logical.GetQueue(graphics_family);
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.cpp' $old.Trim() $new.Trim()


# FidelityFX frame-generation swapchain plumbing.
$old = @'
    VkResult Present(const VkPresentInfoKHR& present_info) const noexcept {
        return dld->vkQueuePresentKHR(queue, &present_info);
    }

private:
'@
$new = @'
    VkResult Present(const VkPresentInfoKHR& present_info) const noexcept {
        return dld->vkQueuePresentKHR(queue, &present_info);
    }

    VkQueue Handle() const noexcept {
        return queue;
    }

private:
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_wrapper.h' $old.Trim() $new.Trim()

$old = @'
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_stall_probe.h"
'@
$new = @'
#include "video_core/renderer_vulkan/scarlet_ffx_framegen.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_stall_probe.h"
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
    VkExtent2D GetExtent() const {
        return extent;
    }

private:
'@
$new = @'
    VkExtent2D GetExtent() const {
        return extent;
    }

    bool FrameGenerationActive() const {
        return ffx_active;
    }

    VkSwapchainKHR RawSwapchain() const {
        return ffx_active ? ffx_swapchain : (swapchain ? *swapchain : VK_NULL_HANDLE);
    }

private:
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.h' $old.Trim() $new.Trim()

$old = @'
    vk::SwapchainKHR swapchain;

    std::size_t image_count{};
'@
$new = @'
    vk::SwapchainKHR swapchain;
    VkSwapchainKHR ffx_swapchain{};
    bool ffx_active{};

    std::size_t image_count{};
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.h' $old.Trim() $new.Trim()

$old = @'
Swapchain::~Swapchain() = default;
'@
$new = @'
Swapchain::~Swapchain() {
    Destroy();
}
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()


# Turn on transfer-source usage when FG is requested, then let the official FidelityFX
# runtime replace the normal Vulkan swapchain. Failure leaves the normal swapchain intact.
$old = @'
    VkSwapchainCreateInfoKHR swapchain_ci{
'@
$new = @'
    const bool frame_generation_requested =
        ScarletPC::Graphics::FidelityFxFrameGeneration::Get().Requested();
    VkSwapchainCreateInfoKHR swapchain_ci{
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
'@
$new = @'
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      ((frame_generation_requested &&
                        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                           ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           : 0),
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
    // Don't add code within this and the swapchain creation.
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
'@
$new = @'
    // Don't add code within this and the normal swapchain creation.
    auto created_swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;
    ffx_active = false;
    ffx_swapchain = VK_NULL_HANDLE;

    if (frame_generation_requested &&
        (swapchain_ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        auto& ffx = ScarletPC::Graphics::FidelityFxFrameGeneration::Get();
        ffx.SetQueues(device.GetGraphicsQueue().Handle(), device.GetGraphicsFamily(),
                      device.GetPresentQueue().Handle(), device.GetPresentFamily());
        VkSwapchainKHR candidate = *created_swapchain;
        if (ffx.CreateSwapchain(device.GetPhysical(), *device.GetLogical(), &candidate,
                                swapchain_ci)) {
#ifndef __ANDROID__
            (void)created_swapchain.release();
#endif
            ffx_swapchain = candidate;
            ffx_active = true;
        } else if (candidate == VK_NULL_HANDLE) {
            // FidelityFX had already consumed the original swapchain before a later
            // initialization step failed. Drop the stale wrapper and create a fresh
            // normal swapchain so Frame Generation can fail without killing the game.
#ifndef __ANDROID__
            (void)created_swapchain.release();
#endif
            created_swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);
        }
    }

    if (ffx_active) {
        u32 count = 0;
        vk::Check(ScarletPC::Graphics::FidelityFxFrameGeneration::Get().GetSwapchainImages(
            *device.GetLogical(), ffx_swapchain, &count, nullptr));
        images.resize(count);
        vk::Check(ScarletPC::Graphics::FidelityFxFrameGeneration::Get().GetSwapchainImages(
            *device.GetLogical(), ffx_swapchain, &count, images.data()));
        images.resize(count);
    } else {
        swapchain = std::move(created_swapchain);
        images = swapchain.GetImages();
        if (frame_generation_requested) {
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] FidelityFX swapchain unavailable; normal presentation active");
        }
    }
    image_count = static_cast<u32>(images.size());
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
        return device.GetLogical().AcquireNextImageKHR(
            *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
            VK_NULL_HANDLE, &image_index);
'@
$new = @'
        if (ffx_active) {
            return ScarletPC::Graphics::FidelityFxFrameGeneration::Get().AcquireNextImage(
                *device.GetLogical(), ffx_swapchain, (std::numeric_limits<u64>::max)(),
                *present_semaphores[frame_index], VK_NULL_HANDLE, &image_index);
        }
        return device.GetLogical().AcquireNextImageKHR(
            *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
            VK_NULL_HANDLE, &image_index);
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
    const VkPresentInfoKHR present_info{
'@
$new = @'
    VkPresentInfoKHR present_info{
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
        .pSwapchains = swapchain.address(),
'@
$new = @'
        .pSwapchains = nullptr,
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
    std::scoped_lock lock{scheduler.submit_mutex};
    const VkResult present_result = [&] {
        StallProbe::Accum probe{StallProbe::present_ns};
        return present_queue.Present(present_info);
    }();
'@
$new = @'
    VkSwapchainKHR present_swapchain = RawSwapchain();
    present_info.pSwapchains = &present_swapchain;

    std::scoped_lock lock{scheduler.submit_mutex};
    const VkResult present_result = [&] {
        StallProbe::Accum probe{StallProbe::present_ns};
        if (ffx_active) {
            return ScarletPC::Graphics::FidelityFxFrameGeneration::Get().QueuePresent(
                device.GetPresentQueue().Handle(), &present_info);
        }
        return present_queue.Present(present_info);
    }();
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()

$old = @'
    render_semaphores.clear();
    swapchain.reset();
}
'@
$new = @'
    render_semaphores.clear();
    images.clear();
    if (ffx_active) {
        ScarletPC::Graphics::FidelityFxFrameGeneration::Get().DestroySwapchain();
        ffx_swapchain = VK_NULL_HANDLE;
        ffx_active = false;
    } else {
        swapchain.reset();
    }
}
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old.Trim() $new.Trim()


# Feed each completed Scarlet frame into the FidelityFX frame-generation prepare pass.
$old = @'
#include "common/settings.h"
#include "common/thread.h"
'@
$new = @'
#include <algorithm>
#include <cstdlib>

#include "common/settings.h"
#include "common/thread.h"
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_present_manager.cpp' $old.Trim() $new.Trim()

$old = @'
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
'@
$new = @'
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/scarlet_ffx_framegen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_present_manager.cpp' $old.Trim() $new.Trim()

$old = @'
    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
'@
$new = @'
    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();

    if (swapchain.FrameGenerationActive()) {
        float frame_ms = 1000.0f / 60.0f;
        if (const char* value = std::getenv("SCARLET_TARGET_FPS"); value && *value) {
            const int fps = std::clamp(std::atoi(value), 30, 240);
            frame_ms = 1000.0f / static_cast<float>(fps);
        }
        if (!ScarletPC::Graphics::FidelityFxFrameGeneration::Get().PrepareCompatibilityFrame(
                device.GetPhysical(), *device.GetLogical(), *cmdbuf, *frame->image,
                swapchain.GetImageFormat(), VkExtent2D{frame->width, frame->height},
                frame_ms)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_WARNING(Render_Vulkan,
                            "[Scarlet.FSR3] Prepare failed; FidelityFX swapchain will present "
                            "the real frames without interpolation");
            }
        }
    }

    const std::array pre_barriers{
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_present_manager.cpp' $old.Trim() $new.Trim()

# Scarlet Launcher graphics/window contract. Unsupported future algorithms remain selectable,
# but the runtime logs and uses an explicit fallback rather than pretending they are active.
$old = @'
    InputCommon::InputSubsystem input_subsystem{};

    // Apply the command line arguments
    system.ApplySettings();
'@
$new = @'
    InputCommon::InputSubsystem input_subsystem{};

    const char* scarlet_window_mode = std::getenv("SCARLET_WINDOW_MODE");
    if (scarlet_window_mode && *scarlet_window_mode) {
        const std::string_view mode{scarlet_window_mode};
        if (mode == "borderless") {
            fullscreen = true;
            Settings::values.fullscreen_mode.SetValue(Settings::FullscreenMode::Borderless);
        } else if (mode == "fullscreen") {
            fullscreen = true;
            Settings::values.fullscreen_mode.SetValue(Settings::FullscreenMode::Exclusive);
        } else {
            fullscreen = false;
        }
    }

    int scarlet_width = 0;
    int scarlet_height = 0;
    if (const char* width = std::getenv("SCARLET_WINDOW_WIDTH"); width && *width) {
        scarlet_width = std::max(0, std::atoi(width));
    }
    if (const char* height = std::getenv("SCARLET_WINDOW_HEIGHT"); height && *height) {
        scarlet_height = std::max(0, std::atoi(height));
    }
    if (scarlet_width > 0 && scarlet_height > 0) {
        const bool ratio_16_10 =
            scarlet_width * 10 == scarlet_height * 16;
        Settings::values.aspect_ratio.SetValue(
            ratio_16_10 ? Settings::AspectRatio::R16_10 : Settings::AspectRatio::R16_9);

        Settings::ResolutionSetup setup = Settings::ResolutionSetup::Res1X;
        if (scarlet_width >= 3800) {
            setup = Settings::ResolutionSetup::Res3X;
        } else if (scarlet_width >= 2500) {
            setup = Settings::ResolutionSetup::Res2X;
        } else if (scarlet_width >= 1900) {
            setup = Settings::ResolutionSetup::Res3_2X;
        } else if (scarlet_width >= 1550) {
            setup = Settings::ResolutionSetup::Res5_4X;
        }
        Settings::values.resolution_setup.SetValue(setup);
        Settings::UpdateRescalingInfo();
    }

    const char* scarlet_upscaler = std::getenv("SCARLET_UPSCALER");
    if (scarlet_upscaler && *scarlet_upscaler) {
        const std::string_view upscaler{scarlet_upscaler};
        if (upscaler == "fsr1") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Fsr);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: FSR 1");
        } else if (upscaler == "fsr3" || upscaler == "fsr31" || upscaler == "fsr4") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Fsr);
            LOG_WARNING(Frontend,
                        "[Scarlet.Graphics] {} requested; current renderer uses FSR 1 fallback",
                        upscaler);
        } else if (upscaler == "dlss") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_WARNING(Frontend,
                        "[Scarlet.Graphics] DLSS requested; current renderer has no DLSS backend, "
                        "using native/bilinear fallback");
        } else {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: native");
        }
    }

    if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY"); quality && *quality) {
        LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler quality request: {}", quality);
    }
    if (const char* fg = std::getenv("SCARLET_FRAME_GENERATION"); fg && *fg && *fg != '0') {
        LOG_WARNING(Frontend,
                    "[Scarlet.Graphics] Frame Generation requested; current renderer has no "
                    "native frame-generation backend yet");
    }

    LOG_INFO(Frontend, "[Scarlet.Graphics] Launch size={}x{} mode={}",
             scarlet_width, scarlet_height,
             scarlet_window_mode && *scarlet_window_mode ? scarlet_window_mode : "windowed");

    // Apply the command line arguments
    system.ApplySettings();
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()


$old = @'
#include <cstdlib>
#include <memory>
'@
$new = @'
#include <algorithm>
#include <cstdlib>
#include <memory>
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

$old = @'
    render_window =
        SDL_CreateWindow(window_title.c_str(),
                         Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height,
                         window_flags);
'@
$new = @'
    int scarlet_window_width = Layout::ScreenUndocked::Width;
    int scarlet_window_height = Layout::ScreenUndocked::Height;
    if (const char* value = std::getenv("SCARLET_WINDOW_WIDTH"); value && *value) {
        scarlet_window_width = std::max(320, std::atoi(value));
    }
    if (const char* value = std::getenv("SCARLET_WINDOW_HEIGHT"); value && *value) {
        scarlet_window_height = std::max(180, std::atoi(value));
    }

    render_window =
        SDL_CreateWindow(window_title.c_str(), scarlet_window_width, scarlet_window_height,
                         window_flags);
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

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
