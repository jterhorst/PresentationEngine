// Core compositor: draws one element as a perspective-correct quad into the
// linear-light, premultiplied RGBA16F output target.
//
// Bindings are explicit so the Metal backend can bind by index:
//   b0 DrawParams, t0 content texture, s0 sampler, t1 backdrop (a copy of the
//   target, for blend modes fixed-function blending can't do) or, for a
//   crossfade, the incoming layer.
//
// SPDX-License-Identifier: MPL-2.0

// Kept in sync with engine::DrawParams (src/core/draw_params.h); static_asserts there.
struct DrawParams
{
    float4 clip[4];        // projected corners TL, TR, BR, BL in clip space, w = perspective divisor
    float4 uv_rect;        // u0, v0, u1, v1 into the content texture
    float4 fill;           // linear, premultiplied
    float4 stroke;         // linear, premultiplied
    float4 shadow;         // linear, premultiplied
    float2 box_size;       // element box in canvas units
    float2 shadow_offset;  // canvas units
    float corner_radius;
    float stroke_width;
    float shadow_blur;
    float opacity;
    uint kind;             // 0 texture, 1 rectangle, 2 ellipse, 3 solid, 4 crossfade t0 -> t1
    uint blend_mode;       // engine.v1.BlendMode; NORMAL and ADD use fixed-function blending
    uint gamma_space;      // 1 = blend in sRGB-encoded space
    uint uses_backdrop;    // 1 = read t1 and write the full composite (blending off)
    float2 target_size;    // pixels
    float aa;              // canvas units per output pixel (edge softness)
    float mix;             // kind 4: weight of t1 against t0
    float2 quad_size;      // drawn quad in canvas units: the box plus room for shadow and stroke
    float2 _pad;
};

ConstantBuffer<DrawParams> draw : register(b0);
Texture2D<float4> content : register(t0);
Texture2D<float4> backdrop : register(t1);
SamplerState linear_clamp : register(s0);

struct VOut
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;   // 0..1 across the box
};

[shader("vertex")]
VOut vs_quad(uint vid : SV_VertexID)
{
    // Triangle strip order: TL, TR, BL, BR.
    static const uint corner_of[4] = { 0, 1, 3, 2 };
    static const float2 local_of[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    VOut o;
    o.pos = draw.clip[corner_of[vid]];
    o.local = local_of[vid];
    return o;
}

float3 to_gamma(float3 c) { return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055); }
float3 to_linear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((max(c, 0.0) + 0.055) / 1.055, 2.4)); }

float sd_round_rect(float2 p, float2 half_size, float r)
{
    float2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sd_ellipse(float2 p, float2 half_size)
{
    // Scaled-circle approximation; exact enough for anti-aliasing an edge.
    float2 n = p / half_size;
    float k = length(n);
    float grad = length(n / half_size);
    return (k - 1.0) / max(grad, 1e-6);
}

float coverage(float d, float aa) { return saturate(0.5 - d / aa); }

float4 shape_color(float2 local)
{
    float2 half_size = draw.box_size * 0.5;
    float2 p = (local - 0.5) * draw.quad_size;
    // One output pixel of edge softness. Slang's Metal target lacks fwidth, so
    // the CPU supplies canvas units per pixel; 3D-tilted shapes soften slightly.
    float aa = max(draw.aa, 1e-4);
    float r = min(draw.corner_radius, min(half_size.x, half_size.y));

    float d = draw.kind == 1 ? sd_round_rect(p, half_size, r) : sd_ellipse(p, half_size);
    float4 result = float4(0);

    if (draw.shadow.a > 0)
    {
        float2 sp = p - draw.shadow_offset;
        float sd = draw.kind == 1 ? sd_round_rect(sp, half_size, r) : sd_ellipse(sp, half_size);
        float blur = max(draw.shadow_blur, aa);
        result = draw.shadow * saturate(0.5 - sd / blur);
    }
    float4 fill = draw.fill * coverage(d, aa);
    result = fill + result * (1 - fill.a);
    if (draw.stroke_width > 0)
    {
        float ds = abs(d) - draw.stroke_width * 0.5;
        float4 s = draw.stroke * coverage(ds, aa);
        result = s + result * (1 - s.a);
    }
    return result;
}

// W3C compositing: separable blend functions on straight colors.
float3 blend_fn(uint mode, float3 cb, float3 cs)
{
    switch (mode)
    {
    case 3: return cb * cs;                                   // multiply
    case 4: return cb + cs - cb * cs;                         // screen
    case 5: return select(cb <= 0.5, 2 * cb * cs, 1 - 2 * (1 - cb) * (1 - cs)); // overlay
    case 6: return min(cb, cs);                               // darken
    case 7: return max(cb, cs);                               // lighten
    case 8: {                                                 // soft light
        float3 d = select(cb <= 0.25, ((16 * cb - 12) * cb + 4) * cb, sqrt(cb));
        return select(cs <= 0.5, cb - (1 - 2 * cs) * cb * (1 - cb), cb + (2 * cs - 1) * (d - cb));
    }
    case 9: return abs(cb - cs);                              // difference
    case 2: return min(cb + cs, 1.0);                         // add
    default: return cs;                                       // normal
    }
}

[shader("fragment")]
float4 fs_composite(VOut i) : SV_Target
{
    float4 src;
    if (draw.kind == 0)
        src = content.Sample(linear_clamp, lerp(draw.uv_rect.xy, draw.uv_rect.zw, i.local)) * draw.fill;
    else if (draw.kind == 3)
        src = draw.fill;
    else if (draw.kind == 4)
        src = lerp(content.Sample(linear_clamp, i.local), backdrop.Sample(linear_clamp, i.local), draw.mix);
    else
        src = shape_color(i.local);
    src *= draw.opacity;

    if (draw.uses_backdrop == 0)
        return src; // fixed-function blend applies NORMAL or ADD

    float4 dst = backdrop.Load(int3(int2(i.pos.xy), 0));
    float3 cs = src.a > 0 ? src.rgb / src.a : float3(0);
    float3 cb = dst.a > 0 ? dst.rgb / dst.a : float3(0);
    if (draw.gamma_space != 0) { cs = to_gamma(cs); cb = to_gamma(cb); }
    float3 mixed = (1 - dst.a) * cs + dst.a * blend_fn(draw.blend_mode, cb, cs);
    if (draw.gamma_space != 0) mixed = to_linear(mixed);
    // Source-over with the blended color.
    float a = src.a + dst.a * (1 - src.a);
    float3 rgb = src.a * mixed + dst.rgb * (1 - src.a);
    return float4(rgb, a);
}
