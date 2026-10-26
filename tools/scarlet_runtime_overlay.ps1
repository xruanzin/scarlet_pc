param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [string]$IconPath,

    [switch]$FastIncremental
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

function Copy-IfDifferent([string]$Source, [string]$Destination) {
    if ((Test-Path $Destination -PathType Leaf)) {
        $srcHash = (Get-FileHash $Source -Algorithm SHA256).Hash
        $dstHash = (Get-FileHash $Destination -Algorithm SHA256).Hash
        if ($srcHash -eq $dstHash) { return }
    }
    Copy-Item $Source $Destination -Force
}

# Scarlet PC owns the software-keyboard frontend used by every swkbd applet call.
$keyboardSourceDir = Join-Path $repoRoot 'runtime\nswitch'
$keyboardTargetDir = Join-Path $SourceDir ('src\' + [string]$provenance.legacy.command_dir)
foreach ($name in @('scarlet_software_keyboard.h', 'scarlet_software_keyboard.cpp')) {
    $src = Join-Path $keyboardSourceDir $name
    $dst = Join-Path $keyboardTargetDir $name
    if (-not (Test-Path $src -PathType Leaf)) {
        throw "Missing Scarlet keyboard source: $src"
    }
    Copy-IfDifferent $src $dst
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
    'scarlet_dlss_pass.cpp',
    'scarlet_ffx_upscale.h',
    'scarlet_ffx_upscale.cpp',
    'scarlet_fsr4_pass.h',
    'scarlet_fsr4_pass.cpp'
)) {
    $src = Join-Path $graphicsSourceDir $name
    $dst = Join-Path $graphicsTargetDir $name
    if (-not (Test-Path $src -PathType Leaf)) {
        throw "Missing Scarlet graphics source: $src"
    }
    Copy-IfDifferent $src $dst
}

$dlssBridgeHeader = Join-Path $repoRoot 'runtime\nvidia\scarlet_dlss_bridge.h'
if (-not (Test-Path $dlssBridgeHeader -PathType Leaf)) {
    throw "Missing Scarlet DLSS bridge ABI header: $dlssBridgeHeader"
}
Copy-IfDifferent $dlssBridgeHeader (Join-Path $graphicsTargetDir 'scarlet_dlss_bridge.h')

$ffxIncludeDir = Join-Path $repoRoot 'third_party\FidelityFX-SDK\ffx-api\include'
if (-not (Test-Path (Join-Path $ffxIncludeDir 'ffx_api\ffx_framegeneration.h') -PathType Leaf)) {
    throw "FidelityFX headers are missing. Run tools\scarlet_fetch_fidelityfx.ps1 first."
}
$ffxIncludeCmake = $ffxIncludeDir.Replace('\', '/')

$fsrVulkanDir = Join-Path $repoRoot 'third_party\FSR-Vulkan'
$fsrVulkanHeader = Join-Path $fsrVulkanDir 'include\ffx_vk_fsr4_v07.h'
if (-not (Test-Path $fsrVulkanHeader -PathType Leaf)) {
    throw "FireBurn FSR-Vulkan provider is missing. Run git submodule update --init --recursive."
}
$fsrVulkanCmake = $fsrVulkanDir.Replace('\', '/')

$vulkanImportLib = Join-Path $repoRoot 'build\tools\vulkan-loader-import\vulkan-1.lib'
if (-not (Test-Path $vulkanImportLib -PathType Leaf)) {
    throw "Vulkan loader import library is missing. Run tools\scarlet_vulkan_loader_import.ps1 first."
}
$vulkanImportCmake = $vulkanImportLib.Replace('\', '/')


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

    # Match multiline patches independently of Git/Windows LF vs CRLF conversion.
    $hadCrLf = $text.Contains("`r`n")
    $textLf = $text.Replace("`r`n", "`n")
    $oldLf = $Old.Replace("`r`n", "`n")
    $newLf = $New.Replace("`r`n", "`n")

    # Additive patches often keep the original anchor inside the replacement. If the
    # replacement is already present, checking/replacing $Old first would apply the same
    # patch again on every quick update and duplicate declarations/functions.
    $replacementContainsAnchor = $newLf.Contains($oldLf)
    if ($replacementContainsAnchor -and $textLf.Contains($newLf)) {
        return
    }

    if ($text.Contains($Old)) {
        $text = $text.Replace($Old, $New)
        [IO.File]::WriteAllText($path, $text, $utf8)
        return
    }

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
    if ($FastIncremental) {
        Write-Host "[Scarlet PC] Quick overlay: skipping already-transformed block in $displayPath" -ForegroundColor DarkGray
        return
    }
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
# Fast incremental builds preserve the already-patched runtime checkout. Later overlay
# steps add provider .c files between these entries, so the original full replacement
# block is no longer a reliable idempotence check. Use a stable Scarlet source as the
# sentinel and only patch the upstream list when it is genuinely absent.
$videoCoreCmake = Join-Path $SourceDir 'src\video_core\CMakeLists.txt'
$videoCoreCmakeText = [IO.File]::ReadAllText($videoCoreCmake)
if (-not $videoCoreCmakeText.Contains('renderer_vulkan/scarlet_ffx_framegen.cpp')) {
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
    renderer_vulkan/scarlet_ffx_upscale.cpp
    renderer_vulkan/scarlet_ffx_upscale.h
    renderer_vulkan/scarlet_fsr4_pass.cpp
    renderer_vulkan/scarlet_fsr4_pass.h
    renderer_vulkan/vk_texture_cache.cpp
'@
    Replace-Literal 'src\video_core\CMakeLists.txt' $old.Trim() $new.Trim()
}

$old = '    renderer_vulkan/scarlet_fsr4_pass.h'
$new = $old + [Environment]::NewLine +
    '    "' + $fsrVulkanCmake + '/src/ffx_vk_fsr4_v07.c"' + [Environment]::NewLine +
    '    "' + $fsrVulkanCmake + '/src/ffx_vk_fsr4_v07_provider.c"' + [Environment]::NewLine +
    '    "' + $fsrVulkanCmake + '/src/ffx_vk_fsr4_v07_assets.c"' + [Environment]::NewLine +
    '    "' + $fsrVulkanCmake + '/src/ffx_vk_fsr4_v07_schedule.c"'
Replace-Literal 'src\video_core\CMakeLists.txt' $old $new

$old = 'target_include_directories(video_core PRIVATE ${HOST_SHADERS_INCLUDE})'
$new = $old + [Environment]::NewLine +
    'target_include_directories(video_core PRIVATE "' + $ffxIncludeCmake + '")' +
    [Environment]::NewLine +
    'target_include_directories(video_core PRIVATE "' + $fsrVulkanCmake + '/include")'
Replace-Literal 'src\video_core\CMakeLists.txt' $old $new

$old = 'target_link_libraries(video_core PRIVATE sirit::sirit)'
$new = $old + [Environment]::NewLine +
    'target_link_libraries(video_core PRIVATE "' + $vulkanImportCmake + '")'
Replace-Literal 'src\video_core\CMakeLists.txt' $old $new


# SpartanPort 2.1.7-style FSR 3.1 path: build FireBurn/FSR-Vulkan's portable
# native Vulkan backend into the runtime instead of loading amd_fidelityfx_vk.dll.
# Keep it in its own target so AMD SDK compile definitions do not leak into suyu.
$videoCoreCmakeText = [IO.File]::ReadAllText($videoCoreCmake)
if ($videoCoreCmakeText.Contains('add_library(scarlet_fsr3_portable STATIC') -and
    -not $videoCoreCmakeText.Contains('SCARLET_FSR230_GEN_UPSCALE')) {
    $old = @'
set(SCARLET_FSR114_GEN_FRAMEGEN
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-1.1.4/vk/fsr3framegeneration")
'@
    $new = @'
set(SCARLET_FSR114_GEN_FRAMEGEN
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-1.1.4/vk/fsr3framegeneration")
set(SCARLET_FSR230_GEN_UPSCALE
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-2.3.0/vk/fsr3upscaler-q2-v2")
'@
    Replace-Literal 'src\video_core\CMakeLists.txt' $old.Trim() $new.Trim()

    $old = @'
        "${SCARLET_FSR114_GEN_UPSCALE}"
        "${SCARLET_FSR114_GEN_FRAMEGEN}"
'@
    $new = @'
        "${SCARLET_FSR114_GEN_UPSCALE}"
        "${SCARLET_FSR114_GEN_FRAMEGEN}"
        "${SCARLET_FSR230_GEN_UPSCALE}"
'@
    Replace-Literal 'src\video_core\CMakeLists.txt' $old.Trim() $new.Trim()
    $videoCoreCmakeText = [IO.File]::ReadAllText($videoCoreCmake)
}
if (-not $videoCoreCmakeText.Contains('add_library(scarlet_fsr3_portable STATIC')) {
    $portableCmake = @'
set(SCARLET_FSR_VULKAN_DIR "__FSR_VULKAN_DIR__")
set(SCARLET_FSR114_ROOT "${SCARLET_FSR_VULKAN_DIR}/upstream/ffx-1.1.4/sdk")
set(SCARLET_FSR114_GEN_UPSCALE
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-1.1.4/vk/fsr3upscaler")
set(SCARLET_FSR114_GEN_FRAMEGEN
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-1.1.4/vk/fsr3framegeneration")
set(SCARLET_FSR230_GEN_UPSCALE
    "${SCARLET_FSR_VULKAN_DIR}/generated/ffx-2.3.0/vk/fsr3upscaler-q2-v2")

add_library(scarlet_fsr3_portable STATIC
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_portable.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_3_1_5_reflection.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_3_1_5_catalog.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_3_1_5_pipeline.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_3_1_5_bundle.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_3_1_5_descriptor.c"
    "${SCARLET_FSR_VULKAN_DIR}/src/ffx_vk_fsr3_portable.cpp"
    "${SCARLET_FSR114_ROOT}/src/shared/ffx_assert.cpp"
    "${SCARLET_FSR114_ROOT}/src/shared/ffx_message.cpp"
    "${SCARLET_FSR114_ROOT}/src/shared/ffx_object_management.cpp"
    "${SCARLET_FSR114_ROOT}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${SCARLET_FSR114_ROOT}/src/components/opticalflow/ffx_opticalflow.cpp"
    "${SCARLET_FSR114_ROOT}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
    "${SCARLET_FSR114_ROOT}/src/components/fsr3/ffx_fsr3.cpp"
    "${SCARLET_FSR114_ROOT}/src/backends/vk/ffx_vk.cpp"
    "${SCARLET_FSR114_ROOT}/src/backends/shared/ffx_shader_blobs.cpp"
    "${SCARLET_FSR114_ROOT}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${SCARLET_FSR114_ROOT}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp"
    "${SCARLET_FSR114_ROOT}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp"
    "${SCARLET_FSR114_ROOT}/src/shared/ffx_breadcrumbs_list.cpp"
)
set_target_properties(scarlet_fsr3_portable PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_definitions(scarlet_fsr3_portable PRIVATE
    FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1
    FFX_FSR3UPSCALER=1
    FFX_FI=1
    FFX_OF=1
)
target_include_directories(scarlet_fsr3_portable
    PUBLIC "${SCARLET_FSR_VULKAN_DIR}/include"
    PRIVATE
        "${SCARLET_FSR114_ROOT}/include"
        "${SCARLET_FSR114_ROOT}/src/shared"
        "${SCARLET_FSR114_ROOT}/src/components"
        "${SCARLET_FSR114_ROOT}/src/backends/shared"
        "${SCARLET_FSR114_ROOT}/src/backends/shared/blob_accessors"
        "${SCARLET_FSR114_GEN_UPSCALE}"
        "${SCARLET_FSR114_GEN_FRAMEGEN}"
        "${SCARLET_FSR230_GEN_UPSCALE}"
)
target_link_libraries(scarlet_fsr3_portable
    PUBLIC Vulkan::Headers
    PRIVATE "__VULKAN_IMPORT_LIB__"
)
if(MSVC)
    # suyu promotes these diagnostics to errors for its own code. FSR-Vulkan is
    # third-party code and SpartanPort builds it without that warnings-as-errors
    # policy, so neutralize only those promoted diagnostics on this target.
    target_compile_options(scarlet_fsr3_portable PRIVATE
        /WX-
        /wd4062 /wd4189 /wd4265 /wd4388 /wd4389
        /wd4456 /wd4457 /wd4458 /wd4459
        /wd4505 /wd4547 /wd4549 /wd4555
        /wd4826 /wd5038 /wd5233 /wd5245 /wd5287
    )
endif()
target_link_libraries(video_core PRIVATE scarlet_fsr3_portable)
'@
    $portableCmake = $portableCmake.Replace('__FSR_VULKAN_DIR__', $fsrVulkanCmake).
        Replace('__VULKAN_IMPORT_LIB__', $vulkanImportCmake)
    [IO.File]::AppendAllText(
        $videoCoreCmake,
        [Environment]::NewLine + $portableCmake.Trim() + [Environment]::NewLine,
        $utf8
    )
}

$old = @'
target_link_libraries(scarlet_fsr3_portable
    PUBLIC Vulkan::Headers
    PRIVATE "__VULKAN_IMPORT_LIB__"
)
target_link_libraries(video_core PRIVATE scarlet_fsr3_portable)
'@
$new = @'
target_link_libraries(scarlet_fsr3_portable
    PUBLIC Vulkan::Headers
    PRIVATE "__VULKAN_IMPORT_LIB__"
)
if(MSVC)
    target_compile_options(scarlet_fsr3_portable PRIVATE
        /WX-
        /wd4062 /wd4189 /wd4265 /wd4388 /wd4389
        /wd4456 /wd4457 /wd4458 /wd4459
        /wd4505 /wd4547 /wd4549 /wd4555
        /wd4826 /wd5038 /wd5233 /wd5245 /wd5287
    )
endif()
target_link_libraries(video_core PRIVATE scarlet_fsr3_portable)
'@
$old = $old.Replace('__VULKAN_IMPORT_LIB__', $vulkanImportCmake)
$new = $new.Replace('__VULKAN_IMPORT_LIB__', $vulkanImportCmake)
Replace-Literal 'src\video_core\CMakeLists.txt' $old.Trim() $new.Trim()


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
LOG_INFO(Frontend, "[Scarlet.Game] Loading Pokemon Scarlet from '{}'", filepath);
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$previousPostLoad = @'
case Core::SystemResultStatus::Success: {
        LOG_INFO(Frontend, "[Scarlet.Game] Pokemon Scarlet loaded");

        // Per-game settings are applied during System::Load and can overwrite the
        // resolution scale selected before boot. Re-apply Scarlet's temporal-FSR
        // render scale now that the renderer is powered on, then refresh its base
        // settings so AccelerateDisplay reports the actually scaled framebuffer.
        const char* postload_upscaler = std::getenv("SCARLET_UPSCALER");
        if (postload_upscaler &&
            (std::string_view{postload_upscaler} == "fsr3" ||
             std::string_view{postload_upscaler} == "fsr31")) {
            int postload_width = 1920;
            if (const char* width = std::getenv("SCARLET_WINDOW_WIDTH"); width && *width) {
                postload_width = std::max(1, std::atoi(width));
            }

            float postload_scale = static_cast<float>(postload_width) / 1920.0f;
            float upscale_ratio = 1.5f;
            if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY");
                quality && *quality) {
                const std::string_view preset{quality};
                if (preset == "balanced") {
                    upscale_ratio = 1.7f;
                } else if (preset == "performance") {
                    upscale_ratio = 2.0f;
                } else if (preset == "ultra_performance") {
                    upscale_ratio = 3.0f;
                }
            }
            postload_scale = std::max(postload_scale / upscale_ratio, 0.5f);

            Settings::ResolutionSetup postload_setup = Settings::ResolutionSetup::Res1X;
            if (postload_scale < 0.625f) {
                postload_setup = Settings::ResolutionSetup::Res1_2X;
            } else if (postload_scale < 0.875f) {
                postload_setup = Settings::ResolutionSetup::Res3_4X;
            } else {
                postload_setup = Settings::ResolutionSetup::Res1X;
            }

            Settings::values.resolution_setup.SetValue(postload_setup);
            Settings::UpdateRescalingInfo();
            system.ApplySettings();
            LOG_INFO(Frontend,
                     "[Scarlet.Graphics] Post-load temporal render scale applied: request={:.3f} actual={:.3f}",
                     postload_scale, Settings::values.resolution_info.up_factor);
        }
        break;
    }
'@
$currentSuccess = @'
case Core::SystemResultStatus::Success:
        LOG_INFO(Frontend, "[Scarlet.Game] Pokemon Scarlet loaded");
        break;
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $previousPostLoad.Trim() $currentSuccess.Trim()

$old = @'
case Core::SystemResultStatus::Success:
        break; // Expected case
'@
$new = @'
case Core::SystemResultStatus::Success:
        LOG_INFO(Frontend, "[Scarlet.Game] Pokemon Scarlet loaded");
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



# Scarlet PC prioritizes reaching gameplay over blocking at startup while Vulkan pipelines
# are precompiled. Keep disk cache storage enabled, but skip the synchronous preload unless
# the user explicitly opts in with SCARLET_PRELOAD_SHADER_CACHE=1.
$old = @'
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        // Build the cached shaders on their own thread, as the Qt frontend does from its
'@
$new = @'
    const char* scarlet_preload_shader_cache = std::getenv("SCARLET_PRELOAD_SHADER_CACHE");
    const bool scarlet_should_preload_shader_cache =
        scarlet_preload_shader_cache && *scarlet_preload_shader_cache != '0';

    if (Settings::values.use_disk_shader_cache.GetValue() && scarlet_should_preload_shader_cache) {
        LOG_INFO(Frontend, "[Scarlet.Shaders] Loading Vulkan disk shader cache");
        // Build the cached shaders on their own thread, as the Qt frontend does from its
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

# Migration for a runtime source that already received the earlier diagnostic-only patch.
$old = @'
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        LOG_INFO(Frontend, "[Scarlet.Shaders] Loading Vulkan disk shader cache");
        // Build the cached shaders on their own thread, as the Qt frontend does from its
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
        loader.join();
        emu_window->HideBuildProgress();
        try {
'@
$new = @'
        loader.join();
        emu_window->HideBuildProgress();
        LOG_INFO(Frontend, "[Scarlet.Shaders] Vulkan disk shader cache complete: {}/{}",
                 built.load(std::memory_order_relaxed), total.load(std::memory_order_relaxed));
        try {
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
    system.RegisterExitCallback([&] {
        // Just exit right away.
        exit(0);
    });
'@
$new = @'
    if (Settings::values.use_disk_shader_cache.GetValue() && !scarlet_should_preload_shader_cache) {
        LOG_INFO(Frontend,
                 "[Scarlet.Shaders] Startup shader-cache preload skipped; pipelines will compile on demand");
    }

    system.RegisterExitCallback([&] {
        // Just exit right away.
        exit(0);
    });
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
    void(system.Run());
    LOG_INFO(Frontend, "[Scarlet.Runtime] Game execution started");
'@
$new = @'
    LOG_INFO(Frontend, "[Scarlet.Runtime] Starting game execution");
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
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' 'return exe.empty() ? std::string{"__PRODUCT__"} : exe.stem().string();' 'return std::string{"Pokemon Scarlet"};'

$old = @'
const std::string window_title = fmt::format("__PRODUCT__ {} | {}-{} (Vulkan)", Common::g_build_name,
                                                 Common::g_scm_branch, Common::g_scm_desc);
'@
$new = @'
const std::string window_title = "Pokemon Scarlet";
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
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Preparing shaders...");' 'std::snprintf(title, sizeof(title), "Pokemon Scarlet | Preparing shaders...");'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Building shaders %zu/%zu", built, total);' 'std::snprintf(title, sizeof(title), "Pokemon Scarlet | Building shaders %zu/%zu", built, total);'

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

# The insertion anchor below remains present after the method is added, so older quick
# overlay runs could prepend TryScarletFastGap again on every update. Derive the exact
# method text from the patch above and collapse any repeated copies in the preserved tree
# before applying/validating the transformation.
$fastGapPath = Join-Path $SourceDir 'src\core\arm\recomp\arm_recomp.cpp'
$fastGapText = [IO.File]::ReadAllText($fastGapPath)
$expandedFastGapPatch = Expand-Legacy $new
$fastGapStart = $expandedFastGapPatch.IndexOf('    bool TryScarletFastGap(u32 opcode) {')
$fastGapEndMarker = '    /// Base address of the module containing `pc`, so an address can be turned'
$fastGapEnd = $expandedFastGapPatch.IndexOf($fastGapEndMarker, $fastGapStart)
if ($fastGapStart -ge 0 -and $fastGapEnd -gt $fastGapStart) {
    $fastGapMethod = $expandedFastGapPatch.Substring($fastGapStart, $fastGapEnd - $fastGapStart).TrimEnd()
    $hadCrLf = $fastGapText.Contains("`r`n")
    $fastGapTextLf = $fastGapText.Replace("`r`n", "`n")
    $fastGapMethodLf = $fastGapMethod.Replace("`r`n", "`n")
    $duplicatePair = $fastGapMethodLf + "`n`n" + $fastGapMethodLf
    $changed = $false
    while ($fastGapTextLf.Contains($duplicatePair)) {
        $fastGapTextLf = $fastGapTextLf.Replace($duplicatePair, $fastGapMethodLf)
        $changed = $true
    }
    if ($changed) {
        if ($hadCrLf) {
            $fastGapTextLf = $fastGapTextLf.Replace("`n", "`r`n")
        }
        [IO.File]::WriteAllText($fastGapPath, $fastGapTextLf, $utf8)
        Write-Host '[Scarlet PC] Quick overlay: collapsed duplicate TryScarletFastGap methods.' -ForegroundColor DarkGray
    }
}

$fastGapText = [IO.File]::ReadAllText($fastGapPath)
if (-not $fastGapText.Contains('    bool TryScarletFastGap(u32 opcode) {')) {
    Replace-Literal 'src\core\arm\recomp\arm_recomp.cpp' $old.Trim() $new.Trim()
}

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

# Older quick-overlay runs could append shaders_compiled repeatedly because the
# replacement retains the shaders_building anchor. Normalize the preserved header first.
$nativeStatusPath = Join-Path $SourceDir ('src\' + [string]$provenance.legacy.command_dir + '\native_status.h')
$nativeStatusText = [IO.File]::ReadAllText($nativeStatusPath)
$nativeStatusLf = $nativeStatusText.Replace("`r`n", "`n")
$compiledFieldPattern = '(?m)^(\s*int shaders_compiled\{\};\s*\n){2,}'
$compiledFieldRegex = New-Object System.Text.RegularExpressions.Regex($compiledFieldPattern)
if ($compiledFieldRegex.IsMatch($nativeStatusLf)) {
    $nativeStatusLf = $compiledFieldRegex.Replace(
        $nativeStatusLf,
        "    int shaders_compiled{};`n",
        1
    )
    if ($nativeStatusText.Contains("`r`n")) {
        $nativeStatusLf = $nativeStatusLf.Replace("`n", "`r`n")
    }
    [IO.File]::WriteAllText($nativeStatusPath, $nativeStatusLf, $utf8)
    Write-Host '[Scarlet PC] Quick overlay: collapsed duplicate native-status shader counter fields.' -ForegroundColor DarkGray
}

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
# Insert the DLC help entry by replacing only the stable --content-probe line.
$old = '                 "--content-probe       Verify that pair and exit without a window or game\n"'
$new = '                 "--content-dlc         Read-only matching DLC NSP/XCI (repeatable)\n"' + "`n" +
       '                 "--content-probe       Verify that pair and exit without a window or game\n"'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old $new
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
            case 'B':
                if (explicit_content_base) {
                    LOG_ERROR(Frontend, "Duplicate --content-base argument");
                    return 2;
                }
                explicit_content_base = optarg;
                break;
'@
$new = @'
            case 'B':
                if (explicit_content_base) {
                    LOG_ERROR(Frontend, "Duplicate --content-base argument");
                    return 2;
                }
#ifdef _WIN32
                explicit_content_base = Common::UTF16ToUTF8(argv_w[optind - 1]);
#else
                explicit_content_base = optarg;
#endif
                break;
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

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
#ifdef _WIN32
                explicit_content_update = Common::UTF16ToUTF8(argv_w[optind - 1]);
#else
                explicit_content_update = optarg;
#endif
                break;
            case 'A':
                if (!optarg || !*optarg) {
                    LOG_ERROR(Frontend, "Invalid --content-dlc argument");
                    return 2;
                }
#ifdef _WIN32
                explicit_content_dlc.emplace_back(Common::UTF16ToUTF8(argv_w[optind - 1]));
#else
                explicit_content_dlc.emplace_back(optarg);
#endif
                break;
            case 'D':
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
            case 'g': {
                const std::string str_arg(optarg);
                filepath = str_arg;
                break;
            }
'@
$new = @'
            case 'g': {
#ifdef _WIN32
                filepath = Common::UTF16ToUTF8(argv_w[optind - 1]);
#else
                filepath = optarg;
#endif
                break;
            }
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

# Older quick-overlay runs could insert AddExplicitDlc repeatedly because its replacement
# keeps the original namespace-close anchor. Collapse any accumulated identical copies in
# the preserved runtime checkout before validating/applying the patch.
$explicitUpdatePath = Join-Path $SourceDir ('src\' + [string]$provenance.legacy.command_dir + '\explicit_update.h')
$explicitUpdateText = [IO.File]::ReadAllText($explicitUpdatePath)
$expandedExplicitDlcPatch = Expand-Legacy $new
$explicitDlcStart = $expandedExplicitDlcPatch.IndexOf('inline bool AddExplicitDlc(')
$explicitDlcEndMarker = '} // namespace SuyuCli'
$explicitDlcEnd = $expandedExplicitDlcPatch.IndexOf($explicitDlcEndMarker, $explicitDlcStart)
if ($explicitDlcStart -ge 0 -and $explicitDlcEnd -gt $explicitDlcStart) {
    $explicitDlcMethod = $expandedExplicitDlcPatch.Substring(
        $explicitDlcStart,
        $explicitDlcEnd - $explicitDlcStart
    ).TrimEnd()

    $hadCrLf = $explicitUpdateText.Contains("`r`n")
    $explicitUpdateLf = $explicitUpdateText.Replace("`r`n", "`n")
    $explicitDlcMethodLf = $explicitDlcMethod.Replace("`r`n", "`n")
    $duplicatePair = $explicitDlcMethodLf + "`n`n" + $explicitDlcMethodLf
    $changed = $false
    while ($explicitUpdateLf.Contains($duplicatePair)) {
        $explicitUpdateLf = $explicitUpdateLf.Replace($duplicatePair, $explicitDlcMethodLf)
        $changed = $true
    }
    if ($changed) {
        if ($hadCrLf) {
            $explicitUpdateLf = $explicitUpdateLf.Replace("`n", "`r`n")
        }
        [IO.File]::WriteAllText($explicitUpdatePath, $explicitUpdateLf, $utf8)
        Write-Host '[Scarlet PC] Quick overlay: collapsed duplicate AddExplicitDlc methods.' -ForegroundColor DarkGray
    }
}

$explicitUpdateText = [IO.File]::ReadAllText($explicitUpdatePath)
if (-not $explicitUpdateText.Contains('inline bool AddExplicitDlc(')) {
    Replace-Literal 'src\__PRODUCT___cmd\explicit_update.h' $old.Trim() $new.Trim()
}

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
#include <variant>
'@
$new = @'
#include <cstdlib>
#include <cstring>
#include <variant>
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()

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
#include "video_core/renderer_vulkan/scarlet_ffx_upscale.h"
#include "video_core/renderer_vulkan/scarlet_fsr4_pass.h"
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
class ScarletFfxUpscalePass;
class ScarletFsr4Pass;

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
    std::unique_ptr<ScarletFfxUpscalePass> ffx_upscale_filter{};
    std::unique_ptr<ScarletFsr4Pass> fsr4_filter{};
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
    if (const char* scarlet_upscaler = std::getenv("SCARLET_UPSCALER");
        scarlet_upscaler &&
        (std::strcmp(scarlet_upscaler, "fsr3") == 0 ||
         std::strcmp(scarlet_upscaler, "fsr31") == 0)) {
        ffx_upscale_filter = std::make_unique<ScarletFfxUpscalePass>(
            device, memory_allocator, image_count, output_size);
    }
    if (const char* scarlet_upscaler = std::getenv("SCARLET_UPSCALER");
        scarlet_upscaler && std::strcmp(scarlet_upscaler, "fsr4") == 0) {
        fsr4_filter = std::make_unique<ScarletFsr4Pass>(
            device, memory_allocator, scheduler, image_count, output_size);
    }
    if (filters.get_scaling_filter() == Settings::ScalingFilter::Fsr) {
        sr_filter.emplace<FSR>(device, memory_allocator, image_count, output_size);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::Sgsr) {
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()

$previousTemporalLayer = @'
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
    } else if (fsr4_filter && fsr4_filter->Ready()) {
        const VkImageView fsr4_view =
            fsr4_filter->Draw(device, scheduler, image_index, source_image,
                              source_image_view, GetFormat(framebuffer), render_extent);
        if (fsr4_view != source_image_view) {
            source_image_view = fsr4_view;
            crop_rect = {0, 0, 1, 1};
        }
    } else if (ffx_upscale_filter && ffx_upscale_filter->Ready()) {
        const VkImageView ffx_view =
            ffx_upscale_filter->Draw(device, scheduler, image_index, source_image,
                                     source_image_view, GetFormat(framebuffer), render_extent);
        if (ffx_view != source_image_view) {
            source_image_view = ffx_view;
            crop_rect = {0, 0, 1, 1};
        }
    }

    if (auto* fsr = std::get_if<FSR>(&sr_filter)) {
'@
$currentTemporalLayer = @'
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
    } else if (fsr4_filter && fsr4_filter->Ready()) {
        const VkImageView fsr4_view =
            fsr4_filter->Draw(device, scheduler, image_index, source_image,
                              source_image_view, GetFormat(framebuffer), render_extent);
        if (fsr4_view != source_image_view) {
            source_image_view = fsr4_view;
            crop_rect = {0, 0, 1, 1};
        }
    } else if (ffx_upscale_filter && ffx_upscale_filter->Ready()) {
        static bool scarlet_logged_temporal_input = false;
        if (!scarlet_logged_temporal_input) {
            scarlet_logged_temporal_input = true;
            const auto& scarlet_resolution = Settings::values.resolution_info;
            LOG_INFO(Render_Vulkan,
                     "[Scarlet.FSR3] Presentation input: framebuffer={}x{} texture={}x{} "
                     "scaled={}x{} resolution_active={} up_factor={:.3f} down_factor={:.3f}",
                     framebuffer.width, framebuffer.height, texture_width, texture_height,
                     scaled_width, scaled_height, scarlet_resolution.active,
                     scarlet_resolution.up_factor, scarlet_resolution.down_factor);
        }
        const VkImageView ffx_view =
            ffx_upscale_filter->Draw(device, scheduler, image_index, source_image,
                                     source_image_view, GetFormat(framebuffer), render_extent);
        if (ffx_view != source_image_view) {
            source_image_view = ffx_view;
            crop_rect = {0, 0, 1, 1};
        }
    }

    if (auto* fsr = std::get_if<FSR>(&sr_filter)) {
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $previousTemporalLayer.Trim() $currentTemporalLayer.Trim()

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
    } else if (fsr4_filter && fsr4_filter->Ready()) {
        const VkImageView fsr4_view =
            fsr4_filter->Draw(device, scheduler, image_index, source_image,
                              source_image_view, GetFormat(framebuffer), render_extent);
        if (fsr4_view != source_image_view) {
            source_image_view = fsr4_view;
            crop_rect = {0, 0, 1, 1};
        }
    } else if (ffx_upscale_filter && ffx_upscale_filter->Ready()) {
        static bool scarlet_logged_temporal_input = false;
        if (!scarlet_logged_temporal_input) {
            scarlet_logged_temporal_input = true;
            const auto& scarlet_resolution = Settings::values.resolution_info;
            LOG_INFO(Render_Vulkan,
                     "[Scarlet.FSR3] Presentation input: framebuffer={}x{} texture={}x{} "
                     "scaled={}x{} resolution_active={} up_factor={:.3f} down_factor={:.3f}",
                     framebuffer.width, framebuffer.height, texture_width, texture_height,
                     scaled_width, scaled_height, scarlet_resolution.active,
                     scarlet_resolution.up_factor, scarlet_resolution.down_factor);
        }
        const VkImageView ffx_view =
            ffx_upscale_filter->Draw(device, scheduler, image_index, source_image,
                                     source_image_view, GetFormat(framebuffer), render_extent);
        if (ffx_view != source_image_view) {
            source_image_view = ffx_view;
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
#include "video_core/renderer_vulkan/scarlet_ffx_framegen.h"
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



# FidelityFX FG queue isolation.
$old = '    [[nodiscard]] Queue GetQueue(u32 family_index) const noexcept;'
$new = '    [[nodiscard]] Queue GetQueue(u32 family_index, u32 queue_index = 0) const noexcept;'
Replace-Literal 'src\video_core\vulkan_common\vulkan_wrapper.h' $old $new

$old = @'
Queue Device::GetQueue(u32 family_index) const noexcept {
    VkQueue queue;
    dld->vkGetDeviceQueue(handle, family_index, 0, &queue);
    return Queue(queue, *dld);
}
'@
$new = @'
Queue Device::GetQueue(u32 family_index, u32 queue_index) const noexcept {
    VkQueue queue;
    dld->vkGetDeviceQueue(handle, family_index, queue_index, &queue);
    return Queue(queue, *dld);
}
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_wrapper.cpp' $old.Trim() $new.Trim()

$old = @'
    /// Returns the main present queue.
    vk::Queue GetPresentQueue() const {
        return present_queue;
    }

    /// Returns main graphics queue family index.
'@
$new = @'
    /// Returns the main present queue.
    vk::Queue GetPresentQueue() const {
        return present_queue;
    }

    /// Returns a present-capable queue reserved exclusively for FidelityFX FG.
    vk::Queue GetFrameGenerationPresentQueue() const {
        return framegen_present_queue;
    }

    bool HasFrameGenerationPresentQueue() const {
        return has_framegen_present_queue;
    }

    /// Returns main graphics queue family index.
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.h' $old.Trim() $new.Trim()

$old = @'
    vk::Queue graphics_queue;    ///< Main graphics queue.
    vk::Queue present_queue;     ///< Main present queue.
    u32 instance_version{};
'@
$new = @'
    vk::Queue graphics_queue;         ///< Main graphics queue.
    vk::Queue present_queue;          ///< Main present queue.
    vk::Queue framegen_present_queue; ///< Queue reserved exclusively for FidelityFX FG.
    bool has_framegen_present_queue{};
    u32 instance_version{};
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.h' $old.Trim() $new.Trim()

$old = @'
std::vector<VkDeviceQueueCreateInfo> Device::GetDeviceQueueCreateInfos() const {
    static constexpr float QUEUE_PRIORITY = 1.0f;

    ankerl::unordered_dense::set<u32> unique_queue_families{graphics_family, present_family};
    std::vector<VkDeviceQueueCreateInfo> queue_cis;
    queue_cis.reserve(unique_queue_families.size());

    for (const u32 queue_family : unique_queue_families) {
        auto& ci = queue_cis.emplace_back(VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = nullptr,
        });
        ci.pQueuePriorities = &QUEUE_PRIORITY;
    }

    return queue_cis;
}
'@
$new = @'
std::vector<VkDeviceQueueCreateInfo> Device::GetDeviceQueueCreateInfos() const {
    static constexpr float QUEUE_PRIORITIES[] = {1.0f, 1.0f};

    const auto queue_family_properties = physical.GetQueueFamilyProperties();
    const bool framegen_requested =
        ScarletPC::Graphics::FidelityFxFrameGeneration::Get().Requested();

    ankerl::unordered_dense::set<u32> unique_queue_families{graphics_family, present_family};
    std::vector<VkDeviceQueueCreateInfo> queue_cis;
    queue_cis.reserve(unique_queue_families.size());

    for (const u32 queue_family : unique_queue_families) {
        u32 requested_count = 1;
        if (framegen_requested && queue_family == present_family &&
            queue_family < queue_family_properties.size() &&
            queue_family_properties[queue_family].queueCount >= 2) {
            const VkQueueFlags flags = queue_family_properties[queue_family].queueFlags;
            if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
                requested_count = 2;
            }
        }

        auto& ci = queue_cis.emplace_back(VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = requested_count,
            .pQueuePriorities = QUEUE_PRIORITIES,
        });
    }

    return queue_cis;
}
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.cpp' $old.Trim() $new.Trim()

$old = @'
    graphics_queue = logical.GetQueue(graphics_family);
    present_queue = logical.GetQueue(present_family);

    VmaVulkanFunctions functions{};
'@
$new = @'
    graphics_queue = logical.GetQueue(graphics_family);
    present_queue = logical.GetQueue(present_family);

    has_framegen_present_queue = false;
    if (ScarletPC::Graphics::FidelityFxFrameGeneration::Get().Requested()) {
        const auto queue_family_properties = physical.GetQueueFamilyProperties();
        if (present_family < queue_family_properties.size() &&
            queue_family_properties[present_family].queueCount >= 2) {
            const VkQueueFlags flags = queue_family_properties[present_family].queueFlags;
            if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
                framegen_present_queue = logical.GetQueue(present_family, 1);
                has_framegen_present_queue = true;
                LOG_INFO(Render_Vulkan,
                         "[Scarlet.FSR3] Reserved dedicated Vulkan present queue family={} index=1 for Frame Generation",
                         present_family);
            }
        }
        if (!has_framegen_present_queue) {
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] No spare present-capable Vulkan queue; Frame Generation swapchain disabled");
        }
    }

    VmaVulkanFunctions functions{};
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
# Keep the Frame Generation request declaration idempotent. Older quick-overlay runs
# could prepend the same declaration repeatedly because the insertion anchor remained in
# the transformed source. Normalize any duplicates in the preserved checkout first.
$swapchainPath = Join-Path $SourceDir 'src\video_core\renderer_vulkan\vk_swapchain.cpp'
$swapchainText = [IO.File]::ReadAllText($swapchainPath)
$fgRequestDecl = @'
    const bool frame_generation_requested =
        ScarletPC::Graphics::FidelityFxFrameGeneration::Get().Requested();
'@.TrimEnd()
$fgDeclPattern = '(?:(?:[ \t]*const bool frame_generation_requested\s*=\s*\r?\n[ \t]*ScarletPC::Graphics::FidelityFxFrameGeneration::Get\(\)\.Requested\(\);\s*\r?\n)){2,}'
$fgDeclRegex = New-Object System.Text.RegularExpressions.Regex(
    $fgDeclPattern,
    [System.Text.RegularExpressions.RegexOptions]::Multiline
)
if ($fgDeclRegex.IsMatch($swapchainText)) {
    $swapchainText = $fgDeclRegex.Replace($swapchainText, $fgRequestDecl + [Environment]::NewLine, 1)
    [IO.File]::WriteAllText($swapchainPath, $swapchainText, $utf8)
}

$swapchainText = [IO.File]::ReadAllText($swapchainPath)
if (-not $swapchainText.Contains($fgRequestDecl)) {
    $old = '    VkSwapchainCreateInfoKHR swapchain_ci{'
    $new = $fgRequestDecl + [Environment]::NewLine + $old
    Replace-Literal 'src\video_core\renderer_vulkan\vk_swapchain.cpp' $old $new
}

$old = @'
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
'@
$new = @'
        .imageUsage = static_cast<VkImageUsageFlags>(
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            ((frame_generation_requested &&
              (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                 : 0)),
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
        (swapchain_ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        device.HasFrameGenerationPresentQueue()) {
        auto& ffx = ScarletPC::Graphics::FidelityFxFrameGeneration::Get();
        ffx.SetQueues(device.GetGraphicsQueue().Handle(), device.GetGraphicsFamily(),
                      device.GetFrameGenerationPresentQueue().Handle(),
                      device.GetPresentFamily());
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
    } else if (frame_generation_requested && !device.HasFrameGenerationPresentQueue()) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR3] Frame Generation requested but no dedicated present queue is available");
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

        // Output/display resolution and temporal render resolution are separate.
        // Changing the monitor/window size must never act like suyu's user-facing
        // 0.5x/2x resolution multiplier. Keep native guest resolution unless a
        // temporal upscaler explicitly needs a lower render target.
        const char* resolution_upscaler = std::getenv("SCARLET_UPSCALER");
        const bool temporal_fsr =
            resolution_upscaler &&
            (std::string_view{resolution_upscaler} == "fsr3" ||
             std::string_view{resolution_upscaler} == "fsr31");

        float fsr_render_scale = 1.0f;
        if (temporal_fsr) {
            float upscale_ratio = 1.5f; // Quality
            if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY");
                quality && *quality) {
                const std::string_view preset{quality};
                if (preset == "balanced") {
                    upscale_ratio = 1.7f;
                } else if (preset == "performance") {
                    upscale_ratio = 2.0f;
                } else if (preset == "ultra_performance") {
                    upscale_ratio = 3.0f;
                }
            }

            // Guest docked rendering is 1920 wide. Convert the selected output
            // mode + FSR quality ratio into a dedicated FSR input scale.
            fsr_render_scale =
                (static_cast<float>(scarlet_width) / 1920.0f) / upscale_ratio;

            // The current texture-cache allocator exposes discrete host scales.
            // 0.5x is the lowest stable temporal input on the target AMD Windows
            // path; arbitrary FSR extents can replace this later without changing
            // the display-resolution contract.
            fsr_render_scale = std::max(fsr_render_scale, 0.5f);
        }

        Settings::ResolutionSetup setup = Settings::ResolutionSetup::Res1X;
        if (temporal_fsr) {
            if (fsr_render_scale < 0.625f) {
                setup = Settings::ResolutionSetup::Res1_2X;
            } else if (fsr_render_scale < 0.875f) {
                setup = Settings::ResolutionSetup::Res3_4X;
            }
        }
        Settings::values.resolution_setup.SetValue(setup);
        Settings::UpdateRescalingInfo();
        LOG_INFO(Frontend,
                 "[Scarlet.Graphics] Output={}x{} fsr_render_scale={:.3f} upscaler={}",
                 scarlet_width, scarlet_height, fsr_render_scale,
                 resolution_upscaler && *resolution_upscaler ? resolution_upscaler : "native");
    }

    const char* scarlet_upscaler = std::getenv("SCARLET_UPSCALER");
    if (scarlet_upscaler && *scarlet_upscaler) {
        const std::string_view upscaler{scarlet_upscaler};
        if (upscaler == "fsr1") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Fsr);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: FSR 1");
        } else if (upscaler == "fsr3" || upscaler == "fsr31") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: {} temporal / FidelityFX",
                     upscaler == "fsr31" ? "FSR 3.1" : "FSR 3");
        } else if (upscaler == "fsr4") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_INFO(Frontend,
                     "[Scarlet.Graphics] Upscaler: FSR 4 v07 experimental Vulkan provider");
        } else if (upscaler == "dlss") {
            // NGX runs as a dedicated Vulkan presentation-layer pass. Keep the built-in
            // spatial scaler neutral so the DLSS output is not filtered twice.
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: NVIDIA DLSS / NGX");
        } else {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
            LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler: native");
        }
    }

    if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY"); quality && *quality) {
        LOG_INFO(Frontend, "[Scarlet.Graphics] Upscaler quality request: {}", quality);
    }
    if (const char* fg = std::getenv("SCARLET_FRAME_GENERATION"); fg && *fg && *fg != '0') {
        LOG_INFO(Frontend,
                 "[Scarlet.Graphics] Frame Generation requested; FidelityFX Vulkan backend enabled");
    }

    LOG_INFO(Frontend, "[Scarlet.Graphics] Launch size={}x{} mode={}",
             scarlet_width, scarlet_height,
             scarlet_window_mode && *scarlet_window_mode ? scarlet_window_mode : "windowed");

    // Apply the command line arguments
    system.ApplySettings();
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

# Fast-incremental migration: older preserved runtime trees already contain the
# previous Scarlet graphics block, so the upstream-anchor replacement above cannot
# update them. Replace that previous transformed block explicitly.
$old = @'
        // The launcher resolution is the OUTPUT resolution. Pick a render scale separately.
        // With an upscaler enabled, reduce the guest render resolution according to the
        // selected quality preset instead of accidentally supersampling it.
        float desired_scale = static_cast<float>(scarlet_width) / 1920.0f;
        const char* resolution_upscaler = std::getenv("SCARLET_UPSCALER");
        const bool uses_upscaler =
            resolution_upscaler && *resolution_upscaler &&
            std::string_view{resolution_upscaler} != "native";
        const bool temporal_fsr =
            resolution_upscaler &&
            (std::string_view{resolution_upscaler} == "fsr3" ||
             std::string_view{resolution_upscaler} == "fsr31");
        if (uses_upscaler) {
            float upscale_ratio = 1.5f; // Quality
            if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY");
                quality && *quality) {
                const std::string_view preset{quality};
                if (preset == "balanced") {
                    upscale_ratio = 1.7f;
                } else if (preset == "performance") {
                    upscale_ratio = 2.0f;
                } else if (preset == "ultra_performance") {
                    upscale_ratio = 3.0f;
                }
            }
            desired_scale /= upscale_ratio;

            // The renderer only exposes discrete guest render scales. At 1366x768,
            // FSR 3 Performance requests ~0.356X; nearest-value rounding selected
            // the experimental 0.25X path, which effectively asks FSR to upscale
            // roughly 2.8x and has proven unstable on the AMD Windows path.
            // Temporal FSR needs at least the 0.5X guest target until arbitrary
            // render extents are supported.
            if (temporal_fsr) {
                desired_scale = std::max(desired_scale, 0.5f);
            }
        }

        Settings::ResolutionSetup setup = Settings::ResolutionSetup::Res1X;
        if (desired_scale < 0.375f) {
            setup = Settings::ResolutionSetup::Res1_4X;
        } else if (desired_scale < 0.625f) {
            setup = Settings::ResolutionSetup::Res1_2X;
        } else if (desired_scale < 0.875f) {
            setup = Settings::ResolutionSetup::Res3_4X;
        } else if (desired_scale < 1.125f) {
            setup = Settings::ResolutionSetup::Res1X;
        } else if (desired_scale < 1.375f) {
            setup = Settings::ResolutionSetup::Res5_4X;
        } else if (desired_scale < 1.75f) {
            setup = Settings::ResolutionSetup::Res3_2X;
        } else if (desired_scale < 2.5f) {
            setup = Settings::ResolutionSetup::Res2X;
        } else if (desired_scale < 3.5f) {
            setup = Settings::ResolutionSetup::Res3X;
        } else {
            setup = Settings::ResolutionSetup::Res4X;
        }
        Settings::values.resolution_setup.SetValue(setup);
        Settings::UpdateRescalingInfo();
        LOG_INFO(Frontend,
                 "[Scarlet.Graphics] Output={}x{} internal_scale_request={:.3f} upscaler={}",
                 scarlet_width, scarlet_height, desired_scale,
                 resolution_upscaler && *resolution_upscaler ? resolution_upscaler : "native");
'@
$new = @'
        // Output/display resolution and temporal render resolution are separate.
        // Changing the monitor/window size must never act like suyu's user-facing
        // 0.5x/2x resolution multiplier. Keep native guest resolution unless a
        // temporal upscaler explicitly needs a lower render target.
        const char* resolution_upscaler = std::getenv("SCARLET_UPSCALER");
        const bool temporal_fsr =
            resolution_upscaler &&
            (std::string_view{resolution_upscaler} == "fsr3" ||
             std::string_view{resolution_upscaler} == "fsr31");

        float fsr_render_scale = 1.0f;
        if (temporal_fsr) {
            float upscale_ratio = 1.5f; // Quality
            if (const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY");
                quality && *quality) {
                const std::string_view preset{quality};
                if (preset == "balanced") {
                    upscale_ratio = 1.7f;
                } else if (preset == "performance") {
                    upscale_ratio = 2.0f;
                } else if (preset == "ultra_performance") {
                    upscale_ratio = 3.0f;
                }
            }

            fsr_render_scale =
                (static_cast<float>(scarlet_width) / 1920.0f) / upscale_ratio;
            fsr_render_scale = std::max(fsr_render_scale, 0.5f);
        }

        Settings::ResolutionSetup setup = Settings::ResolutionSetup::Res1X;
        if (temporal_fsr) {
            if (fsr_render_scale < 0.625f) {
                setup = Settings::ResolutionSetup::Res1_2X;
            } else if (fsr_render_scale < 0.875f) {
                setup = Settings::ResolutionSetup::Res3_4X;
            }
        }
        Settings::values.resolution_setup.SetValue(setup);
        Settings::UpdateRescalingInfo();
        LOG_INFO(Frontend,
                 "[Scarlet.Graphics] Output={}x{} fsr_render_scale={:.3f} upscaler={}",
                 scarlet_width, scarlet_height, fsr_render_scale,
                 resolution_upscaler && *resolution_upscaler ? resolution_upscaler : "native");
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


# Make Scarlet's "fullscreen" mode a real SDL3 exclusive display mode instead of
# silently resizing back to the desktop mode. The requested output resolution is
# therefore a real display mode; "borderless" keeps the desktop mode unchanged.
$old = @'
    case Settings::FullscreenMode::Exclusive:
        // Set window size to render size before entering fullscreen -- SDL3 does not resize window
        // to display dimensions automatically in this mode.
        {
            const SDL_DisplayMode* display_mode =
                SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (display_mode) {
                SDL_SetWindowSize(render_window, display_mode->w, display_mode->h);
            } else {
                LOG_ERROR(Frontend, "SDL_GetDesktopDisplayMode failed: {}", SDL_GetError());
            }
        }

        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());
        LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
        [[fallthrough]];
'@
$new = @'
    case Settings::FullscreenMode::Exclusive:
        {
            int requested_width = 0;
            int requested_height = 0;
            if (const char* value = std::getenv("SCARLET_WINDOW_WIDTH"); value && *value) {
                requested_width = std::max(320, std::atoi(value));
            }
            if (const char* value = std::getenv("SCARLET_WINDOW_HEIGHT"); value && *value) {
                requested_height = std::max(180, std::atoi(value));
            }

            SDL_DisplayID display = SDL_GetDisplayForWindow(render_window);
            if (display == 0) {
                display = SDL_GetPrimaryDisplay();
            }

            SDL_DisplayMode exclusive_mode{};
            const bool have_requested_mode =
                requested_width > 0 && requested_height > 0 &&
                SDL_GetClosestFullscreenDisplayMode(
                    display, requested_width, requested_height, 0.0f, false, &exclusive_mode);

            if (have_requested_mode) {
                if (SDL_SetWindowFullscreenMode(render_window, &exclusive_mode)) {
                    SDL_SetWindowSize(render_window, exclusive_mode.w, exclusive_mode.h);
                    LOG_INFO(Frontend,
                             "[Scarlet.Graphics] Exclusive display mode selected: {}x{} @ {:.3f} Hz",
                             exclusive_mode.w, exclusive_mode.h, exclusive_mode.refresh_rate);
                } else {
                    LOG_ERROR(Frontend, "SDL_SetWindowFullscreenMode failed: {}", SDL_GetError());
                }
            } else {
                const SDL_DisplayMode* desktop_mode = SDL_GetDesktopDisplayMode(display);
                if (desktop_mode) {
                    SDL_SetWindowFullscreenMode(render_window, desktop_mode);
                    SDL_SetWindowSize(render_window, desktop_mode->w, desktop_mode->h);
                    LOG_WARNING(Frontend,
                                "[Scarlet.Graphics] Requested exclusive mode {}x{} unavailable; "
                                "falling back to desktop {}x{}",
                                requested_width, requested_height,
                                desktop_mode->w, desktop_mode->h);
                } else {
                    LOG_ERROR(Frontend, "SDL_GetDesktopDisplayMode failed: {}", SDL_GetError());
                }
            }
        }

        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());
        LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
        [[fallthrough]];
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()


# Exclusive fullscreen focus handling for Windows/AMD:
# own the leave/re-enter sequence explicitly instead of allowing SDL's automatic
# minimize-on-focus-loss to race the Vulkan swapchain/display-mode transition.
$old = @'
    /// Is the window being shown?
    bool is_shown = true;

    /// Internal SDL2 render window
'@
$new = @'
    /// Is the window being shown?
    bool is_shown = true;

    /// Scarlet exclusive fullscreen is temporarily released while another
    /// desktop window owns focus (Win key / Alt+Tab). It is restored only when
    /// the game window gains keyboard focus again.
    bool scarlet_exclusive_focus_suspended = false;

    /// Internal SDL2 render window
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.h' $old.Trim() $new.Trim()

# Do not let SDL auto-minimize exclusive fullscreen while Scarlet performs its
# own synchronous focus transition. This avoids two mode switches racing each other.
$old = @'
    render_window =
        SDL_CreateWindow(window_title.c_str(), scarlet_window_width, scarlet_window_height,
                         window_flags);
'@
$new = @'
    SDL_SetHint("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0");

    render_window =
        SDL_CreateWindow(window_title.c_str(), scarlet_window_width, scarlet_window_height,
                         window_flags);
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

# SDL3 fullscreen/display-mode changes may be asynchronous. Synchronize the
# transition before Vulkan observes the window size or recreates presentation state.
$old = @'
        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }
'@
$new = @'
        if (SDL_SetWindowFullscreen(render_window, true)) {
            if (!SDL_SyncWindow(render_window)) {
                LOG_WARNING(Frontend,
                            "[Scarlet.Graphics] Exclusive fullscreen synchronization timed out: {}",
                            SDL_GetError());
            }
            OnResize();
            return;
        }
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

# On Win-key/Alt+Tab focus loss, release exclusive mode first so Windows can
# restore the desktop mode. Re-enter the requested Scarlet mode only after the
# game regains keyboard focus, then refresh the framebuffer layout so Vulkan
# can rebuild presentation state against the final dimensions.
$old = @'
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
'@
$new = @'
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (!ScarletPC::KeyboardOverlayActive() &&
            Settings::values.fullscreen_mode.GetValue() == Settings::FullscreenMode::Exclusive &&
            (SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN) != 0) {
            scarlet_exclusive_focus_suspended = true;
            LOG_INFO(Frontend,
                     "[Scarlet.Graphics] Focus lost; releasing exclusive fullscreen to desktop");
            if (!SDL_SetWindowFullscreen(render_window, false)) {
                LOG_ERROR(Frontend,
                          "[Scarlet.Graphics] Failed to leave exclusive fullscreen: {}",
                          SDL_GetError());
            } else {
                if (!SDL_SyncWindow(render_window)) {
                    LOG_WARNING(Frontend,
                                "[Scarlet.Graphics] Desktop-mode restore synchronization timed out: {}",
                                SDL_GetError());
                }
                OnResize();
                LOG_INFO(Frontend,
                         "[Scarlet.Graphics] Desktop display mode restored after focus loss");
            }
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (scarlet_exclusive_focus_suspended) {
            scarlet_exclusive_focus_suspended = false;
            LOG_INFO(Frontend,
                     "[Scarlet.Graphics] Focus regained; restoring Scarlet exclusive fullscreen");
            Fullscreen();
            if (!SDL_SyncWindow(render_window)) {
                LOG_WARNING(Frontend,
                            "[Scarlet.Graphics] Fullscreen restore synchronization timed out: {}",
                            SDL_GetError());
            }
            OnResize();
            is_shown = true;
            LOG_INFO(Frontend,
                     "[Scarlet.Graphics] Exclusive fullscreen restored after focus regain");
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

# Coordinate the Win32 Scarlet keyboard with the SDL/main thread. Vulkan exclusive
# fullscreen transitions must not run on the keyboard's detached UI thread.
$old = @'
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/native_status.h"
'@
$new = @'
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/native_status.h"
#include "suyu_cmd/scarlet_software_keyboard.h"
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

$old = @'
void EmuWindow_SDL2::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    // Wait with a deadline rather than forever: with no input there is no event
'@
$new = @'
void EmuWindow_SDL2::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    // The virtual keyboard is a Win32 overlay hosted on a worker thread, but
    // exclusive Vulkan presentation can hide top-level overlays. Keep the game
    // visually fullscreen by switching EXCLUSIVE -> BORDERLESS FULLSCREEN while
    // the keyboard is active. The desktop must never become visible here.
    if (ScarletPC::ConsumeKeyboardDesktopModeRequest()) {
        bool overlay_ready = true;
        if (Settings::values.fullscreen_mode.GetValue() == Settings::FullscreenMode::Exclusive &&
            (SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN) != 0) {
            scarlet_exclusive_focus_suspended = true;
            LOG_INFO(Frontend,
                     "[Scarlet.Keyboard] Switching exclusive fullscreen to borderless keyboard mode");

            // SDL3 can apply fullscreen-mode changes asynchronously. On Windows,
            // changing an already-active exclusive mode directly to nullptr can leave
            // the native HWND temporarily windowed. Perform an explicit, synchronized
            // transition: exclusive -> windowed -> borderless mode -> fullscreen.
            if (!SDL_SetWindowFullscreen(render_window, false)) {
                overlay_ready = false;
                LOG_ERROR(Frontend,
                          "[Scarlet.Keyboard] Failed to leave exclusive mode for keyboard transition: {}",
                          SDL_GetError());
            } else {
                if (!SDL_SyncWindow(render_window)) {
                    LOG_WARNING(Frontend,
                                "[Scarlet.Keyboard] Exclusive-exit synchronization timed out: {}",
                                SDL_GetError());
                }

                if (!SDL_SetWindowFullscreenMode(render_window, nullptr)) {
                    overlay_ready = false;
                    LOG_ERROR(Frontend,
                              "[Scarlet.Keyboard] Failed to select borderless keyboard mode: {}",
                              SDL_GetError());
                } else if (!SDL_SetWindowFullscreen(render_window, true)) {
                    overlay_ready = false;
                    LOG_ERROR(Frontend,
                              "[Scarlet.Keyboard] Failed to enter borderless fullscreen: {}",
                              SDL_GetError());
                } else {
                    if (!SDL_SyncWindow(render_window)) {
                        LOG_WARNING(Frontend,
                                    "[Scarlet.Keyboard] Borderless keyboard-mode synchronization timed out: {}",
                                    SDL_GetError());
                    }

                    const bool is_fullscreen =
                        (SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN) != 0;
                    const bool is_borderless_mode =
                        SDL_GetWindowFullscreenMode(render_window) == nullptr;

                    if (!is_fullscreen || !is_borderless_mode) {
                        overlay_ready = false;
                        LOG_ERROR(Frontend,
                                  "[Scarlet.Keyboard] Borderless verification failed "
                                  "(fullscreen={} borderless_mode={}); restoring exclusive mode",
                                  is_fullscreen, is_borderless_mode);
                        Fullscreen();
                        SDL_SyncWindow(render_window);
                        OnResize();
                        scarlet_exclusive_focus_suspended = false;
                    } else {
                        OnResize();
                        is_shown = true;
                        LOG_INFO(Frontend,
                                 "[Scarlet.Keyboard] Verified borderless fullscreen ready; "
                                 "game remains full-screen behind keyboard");
                    }
                }
            }
        }
        if (overlay_ready) {
            ScarletPC::AcknowledgeKeyboardDesktopMode();
        }
    }

    if (ScarletPC::ConsumeKeyboardFullscreenRestoreRequest()) {
        bool fullscreen_ready = true;
        if (Settings::values.fullscreen_mode.GetValue() == Settings::FullscreenMode::Exclusive &&
            scarlet_exclusive_focus_suspended) {
            LOG_INFO(Frontend,
                     "[Scarlet.Keyboard] Restoring exclusive fullscreen from borderless keyboard mode");
            Fullscreen();
            fullscreen_ready =
                (SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (fullscreen_ready) {
                scarlet_exclusive_focus_suspended = false;
                OnResize();
            } else {
                LOG_ERROR(Frontend,
                          "[Scarlet.Keyboard] Exclusive fullscreen restore did not complete");
            }
        }
        if (fullscreen_ready) {
            ScarletPC::AcknowledgeKeyboardFullscreenRestore();
        }
    }

    // Wait with a deadline rather than forever: with no input there is no event
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

# Do not let a transient owner-focus event restore exclusive mode while the
# keyboard popup is still active.
$old = @'
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (scarlet_exclusive_focus_suspended) {
'@
$new = @'
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (scarlet_exclusive_focus_suspended && !ScarletPC::KeyboardOverlayActive()) {
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' $old.Trim() $new.Trim()

# SDL may not emit a resize event before Vulkan constructs the first presentation
# layer. Seed the framebuffer layout from the actual pixel size immediately so
# FSR's output extent matches the real exclusive mode instead of 1280x720.
$old = @'
    if (fullscreen) {
        Fullscreen();
        ShowCursor(false);
    }

    // Retrieve native window handles via SDL3 property system
'@
$new = @'
    if (fullscreen) {
        Fullscreen();
        ShowCursor(false);
    }
    OnResize();

    // Retrieve native window handles via SDL3 property system
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

# When downscaling is active, ensure the scanout image itself owns the scaled
# host image before presentation. Otherwise the texture cache can keep the
# display image at 1920x1080 even though the render scale is 0.5x, which makes
# the temporal upscaler receive a full-resolution source and disable itself.
$old = @'
    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              slot_images[image_id].IsRescaled());
    };
'@
$new = @'
    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        auto& image = slot_images[image_id];

        // Temporal FSR needs the real scaled host allocation, not merely scaled
        // dimensions in presentation metadata. Force a rescale of the scanout
        // image when the renderer is in a downscale mode and the image is safe
        // to rescale. Subsequent guest writes then target the scaled host image.
        const auto& resolution = Settings::values.resolution_info;
        if (resolution.active && resolution.downscale && !image.IsRescaled() &&
            ImageCanRescale(image)) {
            ScaleUp(image);
        }

        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              image.IsRescaled());
    };
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache.h' $old.Trim() $new.Trim()


$old = @'
    Settings::ScalingFilter scaling_filter{};
    std::unique_ptr<WindowAdaptPass> window_adapt{};
    std::list<Layer> layers{};
};
'@
$new = @'
    Settings::ScalingFilter scaling_filter{};
    std::unique_ptr<WindowAdaptPass> window_adapt{};
    std::list<Layer> layers{};
    VkExtent2D layer_output_size{};
};
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_blit_screen.h' $old.Trim() $new.Trim()


$old = @'
    if (layers.size() != framebuffers.size()) {
        layers.clear();
        for (size_t i = 0; i < framebuffers.size(); ++i) {
            layers.emplace_back(device, memory_allocator, scheduler, device_memory, image_count,
                                window_size, window_adapt->GetDescriptorSetLayout(), filters);
        }
    }
'@
$new = @'
    const bool layer_extent_changed =
        layer_output_size.width != window_size.width ||
        layer_output_size.height != window_size.height;
    if (layers.size() != framebuffers.size() || layer_extent_changed) {
        WaitIdle(device);
        layers.clear();
        layer_output_size = window_size;
        for (size_t i = 0; i < framebuffers.size(); ++i) {
            layers.emplace_back(device, memory_allocator, scheduler, device_memory, image_count,
                                window_size, window_adapt->GetDescriptorSetLayout(), filters);
        }
        LOG_INFO(Render_Vulkan,
                 "[Scarlet.Graphics] Presentation layers rebuilt for {}x{} output",
                 window_size.width, window_size.height);
    }
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_blit_screen.cpp' $old.Trim() $new.Trim()

# Scarlet temporal upscalers own their output extent. When active, use the complete
# host framebuffer as the presentation rectangle instead of MaxRectangle(). This avoids
# 1366x768 being rounded to a 1365x768 16:9 sub-rectangle and prevents a stale
# 1280x720 layout from leaving visible bars around the image.
$old = @'
    auto screen = MaxRectangle(screen_window_area, emulation_aspect_ratio);
    if (window_aspect_ratio < emulation_aspect_ratio) {
        screen = screen.TranslateX((screen_window_area.GetWidth() - screen.GetWidth()) / 2);
    } else {
        screen = screen.TranslateY((height - screen.GetHeight()) / 2);
    }
'@
$new = @'
    Common::Rectangle<u32> screen{};
    const char* scarlet_upscaler = std::getenv("SCARLET_UPSCALER");
    const bool scarlet_temporal_output =
        scarlet_upscaler &&
        (std::string_view{scarlet_upscaler} == "fsr3" ||
         std::string_view{scarlet_upscaler} == "fsr31" ||
         std::string_view{scarlet_upscaler} == "fsr4" ||
         std::string_view{scarlet_upscaler} == "dlss");
    if (scarlet_temporal_output) {
        screen = screen_window_area;
    } else {
        screen = MaxRectangle(screen_window_area, emulation_aspect_ratio);
        if (window_aspect_ratio < emulation_aspect_ratio) {
            screen = screen.TranslateX((screen_window_area.GetWidth() - screen.GetWidth()) / 2);
        } else {
            screen = screen.TranslateY((height - screen.GetHeight()) / 2);
        }
    }
'@
Replace-Literal 'src\core\frontend\framebuffer_layout.cpp' $old.Trim() $new.Trim()

# framebuffer_layout.cpp now inspects the Scarlet upscaler environment.
$old = @'
#include <cmath>

#include "common/assert.h"
'@
$new = @'
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "common/assert.h"
'@
Replace-Literal 'src\core\frontend\framebuffer_layout.cpp' $old.Trim() $new.Trim()

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

# --- Scarlet FSR3 real temporal input bridge ---------------------------------
# Capture a real depth target plus a compatible 2-channel floating-point MRT
# from the texture cache and forward them to the presentation FSR pass. The
# adapter only accumulates history when both inputs match the scaled scene size.

$old = @'
struct FramebufferTextureInfo {
    VkImage image{};
    VkImageView image_view{};
    u32 width{};
    u32 height{};
    u32 scaled_width{};
    u32 scaled_height{};
};
'@
$new = @'
struct FramebufferTextureInfo {
    VkImage image{};
    VkImageView image_view{};
    u32 width{};
    u32 height{};
    u32 scaled_width{};
    u32 scaled_height{};

    VkImage depth_image{};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags depth_aspect{VK_IMAGE_ASPECT_DEPTH_BIT};
    u32 depth_width{};
    u32 depth_height{};

    VkImage motion_image{};
    VkFormat motion_format{VK_FORMAT_UNDEFINED};
    u32 motion_width{};
    u32 motion_height{};
};
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_blit_screen.h' $old.Trim() $new.Trim()

$old = @'
    [[nodiscard]] std::pair<ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig& config, DAddr cpu_addr);

    /// Return true when there are uncommitted images to be downloaded
'@
$new = @'
    [[nodiscard]] std::pair<ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig& config, DAddr cpu_addr);

    /// Scarlet temporal reconstruction inputs captured from the most recent
    /// scene MRT set that exposed both depth and a 2-channel float motion target.
    [[nodiscard]] std::pair<ImageView*, ImageView*> TryFindScarletTemporalImageViews();

    /// Return true when there are uncommitted images to be downloaded
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache_base.h' $old.Trim() $new.Trim()

$old = @'
    RenderTargets render_targets;
    u64 render_targets_serial = 0;
'@
$new = @'
    RenderTargets render_targets;
    ImageViewId scarlet_temporal_depth_view_id{};
    ImageViewId scarlet_temporal_motion_view_id{};
    u64 render_targets_serial = 0;
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache_base.h' $old.Trim() $new.Trim()

# Remember the last scene-like MRT pair. R16G16_FLOAT/R32G32_FLOAT are the
# portable FSR formats appropriate for motion vectors; keeping the pair across
# later post-processing/UI passes lets presentation still consume the scene data.
$old = @'
    render_targets.is_rescaled = is_rescaling;

    if (render_targets != previous_render_targets) {
'@
$new = @'
    render_targets.is_rescaled = is_rescaling;

    // Diagnose Scarlet's real scene MRT layout so the temporal bridge can select
    // the game's actual velocity target instead of guessing. Keep this bounded:
    // render-target state can change thousands of times per frame.
    if (depth_buffer_id) {
        static u32 scarlet_temporal_probe_logs = 0;
        if (scarlet_temporal_probe_logs < 96) {
            const auto& scarlet_probe_depth = slot_image_views[depth_buffer_id];
            LOG_INFO(HW_GPU,
                     "[Scarlet.FSR3.Probe] depth format={} extent={}x{} active_mask={:#x}",
                     static_cast<u32>(scarlet_probe_depth.format),
                     scarlet_probe_depth.size.width, scarlet_probe_depth.size.height,
                     rt_active_mask);
            for (size_t scarlet_probe_rt = 0;
                 scarlet_probe_rt < render_targets.color_buffer_ids.size();
                 ++scarlet_probe_rt) {
                if ((rt_active_mask & (1u << scarlet_probe_rt)) == 0) {
                    continue;
                }
                const ImageViewId scarlet_probe_id =
                    render_targets.color_buffer_ids[scarlet_probe_rt];
                if (!scarlet_probe_id) {
                    continue;
                }
                const auto& scarlet_probe_view = slot_image_views[scarlet_probe_id];
                LOG_INFO(HW_GPU,
                         "[Scarlet.FSR3.Probe]   rt{} format={} extent={}x{}",
                         scarlet_probe_rt, static_cast<u32>(scarlet_probe_view.format),
                         scarlet_probe_view.size.width, scarlet_probe_view.size.height);
            }
            ++scarlet_temporal_probe_logs;
        }

        auto& scarlet_depth = slot_image_views[depth_buffer_id];
        for (size_t scarlet_rt = 0; scarlet_rt < render_targets.color_buffer_ids.size();
             ++scarlet_rt) {
            if ((rt_active_mask & (1u << scarlet_rt)) == 0) {
                continue;
            }
            const ImageViewId color_id = render_targets.color_buffer_ids[scarlet_rt];
            if (!color_id) {
                continue;
            }
            auto& scarlet_motion = slot_image_views[color_id];
            const bool motion_format =
                scarlet_motion.format == PixelFormat::R16G16_FLOAT ||
                scarlet_motion.format == PixelFormat::R32G32_FLOAT;
            const bool same_extent =
                scarlet_motion.size.width == scarlet_depth.size.width &&
                scarlet_motion.size.height == scarlet_depth.size.height;
            if (motion_format && same_extent) {
                scarlet_temporal_depth_view_id = depth_buffer_id;
                scarlet_temporal_motion_view_id = color_id;
                static bool scarlet_logged_temporal_candidate = false;
                if (!scarlet_logged_temporal_candidate) {
                    scarlet_logged_temporal_candidate = true;
                    LOG_INFO(HW_GPU,
                             "[Scarlet.FSR3.Probe] selected temporal candidate rt{} format={} depth_format={} extent={}x{}",
                             scarlet_rt, static_cast<u32>(scarlet_motion.format),
                             static_cast<u32>(scarlet_depth.format),
                             scarlet_motion.size.width, scarlet_motion.size.height);
                }
                break;
            }
        }
    }

    if (render_targets != previous_render_targets) {
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache.h' $old.Trim() $new.Trim()

$old = @'
    return {};
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
'@
$new = @'
    return {};
}

template <class P>
std::pair<typename P::ImageView*, typename P::ImageView*>
TextureCache<P>::TryFindScarletTemporalImageViews() {
    if (!scarlet_temporal_depth_view_id || !scarlet_temporal_motion_view_id) {
        return {};
    }
    return {
        &slot_image_views[scarlet_temporal_depth_view_id],
        &slot_image_views[scarlet_temporal_motion_view_id],
    };
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache.h' $old.Trim() $new.Trim()

# Any view deletion/invalidation must invalidate the retained temporal pair.
$old = @'
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
'@
$new = @'
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
        if (scarlet_temporal_depth_view_id == image_view_id) {
            scarlet_temporal_depth_view_id = ImageViewId{};
        }
        if (scarlet_temporal_motion_view_id == image_view_id) {
            scarlet_temporal_motion_view_id = ImageViewId{};
        }
'@
Replace-Literal 'src\video_core\texture_cache\texture_cache.h' $old.Trim() $new.Trim()

$old = @'
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    return info;
}
'@
$new = @'
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;

    const auto [depth_view, motion_view] =
        texture_cache.TryFindScarletTemporalImageViews();
    if (depth_view && motion_view) {
        const bool depth_scaled = depth_view->IsRescaled();
        const bool motion_scaled = motion_view->IsRescaled();
        const auto depth_format =
            MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, depth_view->format).format;
        const auto motion_format =
            MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, motion_view->format).format;

        info.depth_image = depth_view->ImageHandle();
        info.depth_format = depth_format;
        info.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        info.depth_width =
            depth_scaled ? resolution.ScaleUp(depth_view->size.width) : depth_view->size.width;
        info.depth_height =
            depth_scaled ? resolution.ScaleUp(depth_view->size.height) : depth_view->size.height;

        info.motion_image = motion_view->ImageHandle();
        info.motion_format = motion_format;
        info.motion_width =
            motion_scaled ? resolution.ScaleUp(motion_view->size.width) : motion_view->size.width;
        info.motion_height =
            motion_scaled ? resolution.ScaleUp(motion_view->size.height) : motion_view->size.height;
    }
    return info;
}
'@
Replace-Literal 'src\video_core\renderer_vulkan\vk_rasterizer.cpp' $old.Trim() $new.Trim()

# Feed those real Vulkan images into Scarlet's FSR bridge.
$old = @'
        const VkImageView ffx_view =
            ffx_upscale_filter->Draw(device, scheduler, image_index, source_image,
                                     source_image_view, GetFormat(framebuffer), render_extent);
'@
$new = @'
        ScarletFfxTemporalInputs temporal_inputs{};
        if (texture_info) {
            temporal_inputs.depth_image = texture_info->depth_image;
            temporal_inputs.depth_format = texture_info->depth_format;
            temporal_inputs.depth_aspect = texture_info->depth_aspect;
            temporal_inputs.depth_extent =
                VkExtent2D{texture_info->depth_width, texture_info->depth_height};
            temporal_inputs.motion_image = texture_info->motion_image;
            temporal_inputs.motion_format = texture_info->motion_format;
            temporal_inputs.motion_extent =
                VkExtent2D{texture_info->motion_width, texture_info->motion_height};
        }
        const VkImageView ffx_view =
            ffx_upscale_filter->Draw(device, scheduler, image_index, source_image,
                                     source_image_view, GetFormat(framebuffer), render_extent,
                                     temporal_inputs);
'@
Replace-Literal 'src\video_core\renderer_vulkan\present\layer.cpp' $old.Trim() $new.Trim()

Write-Host '[Scarlet PC] Runtime overlay applied.' -ForegroundColor Green
