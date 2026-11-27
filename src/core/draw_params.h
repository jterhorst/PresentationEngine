// Per-draw constants for shaders/core/composite.slang. Plain data; the layout
// must match the shader's DrawParams exactly (checked below).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

struct float4 { float x = 0, y = 0, z = 0, w = 0; };
struct float2 { float x = 0, y = 0; };

enum class DrawKind : uint32_t { Texture = 0, Rectangle = 1, Ellipse = 2, Solid = 3, Crossfade = 4 };

struct DrawParams {
    float4 clip[4];        // projected corners TL, TR, BR, BL in clip space
    float4 uv_rect{0, 0, 1, 1};
    float4 fill{1, 1, 1, 1};  // linear premultiplied; a tint for textures
    float4 stroke;
    float4 shadow;
    float2 box_size;
    float2 shadow_offset;
    float corner_radius = 0;
    float stroke_width = 0;
    float shadow_blur = 0;
    float opacity = 1;
    uint32_t kind = 0;
    uint32_t blend_mode = 1;
    uint32_t gamma_space = 0;
    uint32_t uses_backdrop = 0;
    float2 target_size;
    float aa = 1;
    float mix = 0;
    float2 quad_size;
    float2 _pad;
};

static_assert(sizeof(DrawParams) == 208, "DrawParams must match composite.slang");
static_assert(offsetof(DrawParams, box_size) == 128);
static_assert(offsetof(DrawParams, kind) == 160);
static_assert(offsetof(DrawParams, target_size) == 176);
static_assert(offsetof(DrawParams, quad_size) == 192);

// Matches ConvertParams in shaders/core/convert.slang.
struct ConvertParams {
    uint32_t width = 0, height = 0;
    uint32_t row_words = 0;
    uint32_t background = 1;
};
static_assert(sizeof(ConvertParams) == 16);

}  // namespace engine
