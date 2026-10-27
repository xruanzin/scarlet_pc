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
        throw "Scarlet FPS overlay: missing runtime source $RelativePath"
    }
    return [IO.File]::ReadAllText($path)
}

function Write-Source([string]$RelativePath, [string]$Text) {
    $path = Join-Path $SourceDir $RelativePath
    [IO.File]::WriteAllText($path, $Text, $utf8)
}

$passRel = 'src\video_core\renderer_vulkan\present\window_adapt_pass.cpp'
$pass = Read-Source $passRel

if (-not $pass.Contains('[Scarlet.FPS] presentation overlay')) {
    if (-not $pass.Contains('#include <chrono>')) {
        $includeAnchor = '#include "core/frontend/framebuffer_layout.h"'
        if (-not $pass.Contains($includeAnchor)) {
            throw 'Scarlet FPS overlay: include anchor missing'
        }
        $includeText = '#include <chrono>' + [Environment]::NewLine +
                       '#include <cstdlib>' + [Environment]::NewLine + [Environment]::NewLine +
                       $includeAnchor
        $pass = $pass.Replace($includeAnchor, $includeText)
    }

    $drawPattern =
        '(?m)^(?<indent>[ \t]*)const\s+size_t\s+layer_count\s*=\s*configs\.size\(\)\s*;\s*$'
    $drawRegex = [regex]::new(
        $drawPattern,
        [Text.RegularExpressions.RegexOptions]::Multiline
    )
    if (-not $drawRegex.IsMatch($pass)) {
        throw 'Scarlet FPS overlay: WindowAdaptPass layer_count anchor missing'
    }
    $drawInsert = @'
${indent}// [Scarlet.FPS] presentation overlay
${indent}// Enabled by default. Set SCARLET_FPS_OVERLAY=0 to hide it.
${indent}static const bool scarlet_fps_overlay = [] {
${indent}    const char* value = std::getenv("SCARLET_FPS_OVERLAY");
${indent}    return value == nullptr || *value == '\0' || *value != '0';
${indent}}();

${indent}static thread_local auto scarlet_fps_window_start = std::chrono::steady_clock::now();
${indent}static thread_local u32 scarlet_fps_frames = 0;
${indent}static thread_local f32 scarlet_fps_value = 0.0f;
${indent}++scarlet_fps_frames;
${indent}const auto scarlet_fps_now = std::chrono::steady_clock::now();
${indent}const auto scarlet_fps_elapsed =
${indent}    std::chrono::duration<f32>(scarlet_fps_now - scarlet_fps_window_start).count();
${indent}if (scarlet_fps_elapsed >= 0.25f) {
${indent}    scarlet_fps_value =
${indent}        static_cast<f32>(scarlet_fps_frames) / scarlet_fps_elapsed;
${indent}    scarlet_fps_frames = 0;
${indent}    scarlet_fps_window_start = scarlet_fps_now;
${indent}}

${indent}const size_t layer_count = configs.size();
'@
    $pass = $drawRegex.Replace(
        $pass,
        $drawInsert.TrimEnd(),
        1
    )

    $configurePattern =
        '(?s)(?<call>layer_it->ConfigureDraw\s*\(\s*device\s*,\s*&push_constants\[i\]\s*,\s*' +
        '&descriptor_sets\[i\]\s*,\s*rasterizer\s*,\s*\*sampler\s*,\s*image_index\s*,\s*' +
        'configs\[i\]\s*,\s*layout\s*\)\s*;)'
    $configureRegex = [regex]::new(
        $configurePattern,
        [Text.RegularExpressions.RegexOptions]::Singleline
    )
    if (-not $configureRegex.IsMatch($pass)) {
        throw 'Scarlet FPS overlay: ConfigureDraw semantic anchor missing'
    }
    $configureInsert = @'
${call}

        // The third matrix column multiplies guest vertex Z (always zero).
        // Reuse it as fragment-stage metadata without growing push constants.
        const bool scarlet_draw_fps = scarlet_fps_overlay && (i + 1 == layer_count);
        push_constants[i].modelview_matrix[8] =
            scarlet_draw_fps ? scarlet_fps_value : -1.0f;
        push_constants[i].modelview_matrix[9] = static_cast<f32>(render_area.width);
        push_constants[i].modelview_matrix[10] = static_cast<f32>(render_area.height);
        push_constants[i].modelview_matrix[11] = scarlet_draw_fps ? 1.0f : 0.0f;
'@
    $pass = $configureRegex.Replace(
        $pass,
        $configureInsert.TrimEnd(),
        1
    )

    if (-not $pass.Contains('VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT')) {
        $stagePattern = '\.stageFlags\s*=\s*VK_SHADER_STAGE_VERTEX_BIT\s*,'
        $stageRegex = [regex]::new($stagePattern)
        if (-not $stageRegex.IsMatch($pass)) {
            throw 'Scarlet FPS overlay: push-constant stage semantic anchor missing'
        }
        $pass = $stageRegex.Replace(
            $pass,
            '.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,',
            1
        )
    }

    Write-Source $passRel $pass
    Write-Host '[Scarlet PC] Native Vulkan FPS overlay host path installed.' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] Native Vulkan FPS overlay host path already installed.' -ForegroundColor DarkGray
}

$shaderRel = 'src\video_core\host_shaders\vulkan_present.frag'
$shader = Read-Source $shaderRel

if (-not $shader.Contains('[Scarlet.FPS] procedural counter')) {
    $oldShader = @'
// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460 core

layout (location = 0) in vec2 frag_tex_coord;

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D color_texture;

void main() {
    color = texture(color_texture, frag_tex_coord);
}
'@

    $newShader = @'
// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460 core

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;
layout (binding = 0) uniform sampler2D color_texture;

struct ScreenRectVertex {
    vec2 position;
    vec2 tex_coord;
};

layout (push_constant) uniform PushConstants {
    mat4 modelview_matrix;
    ScreenRectVertex vertices[4];
};

uint ScarletGlyph(int code) {
    // [Scarlet.FPS] procedural counter
    switch (code) {
    case 0: return 0x7B6Fu;
    case 1: return 0x749Au;
    case 2: return 0x73E7u;
    case 3: return 0x79E7u;
    case 4: return 0x49EDu;
    case 5: return 0x79CFu;
    case 6: return 0x7BCFu;
    case 7: return 0x24A7u;
    case 8: return 0x7BEFu;
    case 9: return 0x79EFu;
    case 10: return 0x12CFu;
    case 11: return 0x12EBu;
    case 12: return 0x79CFu;
    default: return 0u;
    }
}

bool ScarletGlyphPixel(int code, ivec2 p) {
    if (p.x < 0 || p.x >= 3 || p.y < 0 || p.y >= 5) {
        return false;
    }
    const uint bit = uint(p.y * 3 + p.x);
    return ((ScarletGlyph(code) >> bit) & 1u) != 0u;
}

int ScarletCharAt(int index, int fps) {
    if (index == 0) return 10;
    if (index == 1) return 11;
    if (index == 2) return 12;
    if (index == 3) return -1;

    const int value = clamp(fps, 0, 999);
    const int hundreds = value / 100;
    const int tens = (value / 10) % 10;
    const int ones = value % 10;

    if (index == 4) return value >= 100 ? hundreds : -1;
    if (index == 5) return value >= 10 ? tens : -1;
    if (index == 6) return ones;
    return -1;
}

void main() {
    vec4 base = texture(color_texture, frag_tex_coord);
    color = base;

    const float overlay_enabled = modelview_matrix[2][3];
    const float fps_value = modelview_matrix[2][0];
    const float output_width = modelview_matrix[2][1];
    const float output_height = modelview_matrix[2][2];

    if (overlay_enabled < 0.5 || fps_value < 0.0 ||
        output_width <= 0.0 || output_height <= 0.0) {
        return;
    }

    const float scale = 3.0;
    const float glyph_w = 3.0 * scale;
    const float glyph_h = 5.0 * scale;
    const float advance = 4.0 * scale;
    const float margin = 10.0;
    const float pad_x = 8.0;
    const float pad_y = 6.0;
    const float text_w = 7.0 * advance - scale;
    const float box_w = text_w + 2.0 * pad_x;
    const float box_h = glyph_h + 2.0 * pad_y;

    vec2 frag = gl_FragCoord.xy;
    const vec2 box_min = vec2(output_width - margin - box_w, margin);
    const vec2 box_max = box_min + vec2(box_w, box_h);

    if (frag.x < box_min.x || frag.x >= box_max.x ||
        frag.y < box_min.y || frag.y >= box_max.y) {
        return;
    }

    color.rgb = mix(base.rgb, vec3(0.015), 0.78);

    const vec2 text_origin = box_min + vec2(pad_x, pad_y);
    const vec2 local = frag - text_origin;
    if (local.x < 0.0 || local.y < 0.0 ||
        local.x >= text_w || local.y >= glyph_h) {
        return;
    }

    const int char_index = int(floor(local.x / advance));
    if (char_index < 0 || char_index >= 7) {
        return;
    }

    const float char_x = local.x - float(char_index) * advance;
    if (char_x >= glyph_w) {
        return;
    }

    const ivec2 pixel = ivec2(floor(char_x / scale), floor(local.y / scale));
    const int fps_int = int(floor(fps_value + 0.5));
    const int code = ScarletCharAt(char_index, fps_int);

    if (code >= 0 && ScarletGlyphPixel(code, pixel)) {
        color.rgb = vec3(1.0);
    }
}
'@

    if ($shader.Contains($oldShader.Trim())) {
        $shader = $shader.Replace($oldShader.Trim(), $newShader.Trim())
    } else {
        $mainPattern =
            '(?s)void\s+main\s*\(\s*\)\s*\{\s*color\s*=\s*texture\s*\(\s*color_texture\s*,\s*frag_tex_coord\s*\)\s*;\s*\}'
        $mainRegex = [regex]::new(
            $mainPattern,
            [Text.RegularExpressions.RegexOptions]::Singleline
        )
        if (-not $mainRegex.IsMatch($shader)) {
            throw 'Scarlet FPS overlay: vulkan_present.frag semantic main anchor missing'
        }

        $shaderPrefix = $newShader.Substring(0, $newShader.IndexOf('void main()'))
        $helperStart = $shaderPrefix.IndexOf('struct ScreenRectVertex')
        $helpers = $shaderPrefix.Substring($helperStart)
        if (-not $shader.Contains('[Scarlet.FPS] procedural counter')) {
            $mainIndex = $shader.IndexOf('void main()')
            $shader = $shader.Insert($mainIndex, $helpers + [Environment]::NewLine)
        }

        $newMain = $newShader.Substring($newShader.IndexOf('void main()'))
        $shader = $mainRegex.Replace($shader, $newMain.Trim(), 1)
    }
    Write-Source $shaderRel $shader
    Write-Host '[Scarlet PC] Native Vulkan FPS badge shader installed.' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] Native Vulkan FPS badge shader already installed.' -ForegroundColor DarkGray
}
