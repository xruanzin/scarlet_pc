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
$keyboardTargetDir = Join-Path $SourceDir 'src\__PRODUCT___cmd'
foreach ($name in @('scarlet_software_keyboard.h', 'scarlet_software_keyboard.cpp')) {
    $src = Join-Path $keyboardSourceDir $name
    $dst = Join-Path (Expand-Legacy $keyboardTargetDir) $name
    if (-not (Test-Path $src -PathType Leaf)) {
        throw "Missing Scarlet keyboard source: $src"
    }
    Copy-Item $src $dst -Force
}

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

# Scarlet runtime logs keep categories/levels/messages but hide upstream source paths.
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
