// Output conversion: one compute pass from the linear, premultiplied RGBA16F
// composite to the output's wire format (architecture.md, step 6 "Convert").
//
// Screens get sRGB-encoded BGRA8. SDI gets 8-bit UYVY or 10-bit v210, using
// Rec. 709 limited-range coefficients on sRGB-encoded values. The CPU
// reference in src/render/convert_reference.h must match these kernels.
//
// Bindings: b0 ConvertParams, t0 source composite, u1 destination.
//
// SPDX-License-Identifier: MPL-2.0

struct ConvertParams
{
    uint2 size;          // source pixels
    uint row_words;      // destination row stride in 32-bit words (UYVY, v210)
    uint background;     // 1 = composite over opaque black (SDI fill, screens)
};

ConstantBuffer<ConvertParams> params : register(b0);
Texture2D<float4> source : register(t0);

float3 encode_srgb(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float4 load_display(uint2 p)
{
    float4 c = source.Load(int3(int2(p), 0));
    // Premultiplied over black is just the premultiplied color.
    return float4(encode_srgb(c.rgb), params.background != 0 ? 1.0 : c.a);
}

// Rec. 709, limited range, normalized 0..1 -> code values scaled later.
float3 rgb_to_ycbcr(float3 rgb)
{
    float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float3(16.0 / 255.0 + y * 219.0 / 255.0, 128.0 / 255.0 + cb * 224.0 / 255.0, 128.0 / 255.0 + cr * 224.0 / 255.0);
}

WTexture2D<float4> bgra_out : register(u1);

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_to_bgra8(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= params.size)) return;
    bgra_out.Store(id.xy, load_display(id.xy));
}

RWStructuredBuffer<uint> words_out : register(u2);

// One thread per pixel pair: bytes U Y0 V Y1 in memory order.
[shader("compute")]
[numthreads(16, 16, 1)]
void cs_to_uyvy(uint3 id : SV_DispatchThreadID)
{
    uint2 p = uint2(id.x * 2, id.y);
    if (p.x >= params.size.x || p.y >= params.size.y) return;
    float3 a = rgb_to_ycbcr(load_display(p).rgb);
    float3 b = rgb_to_ycbcr(load_display(uint2(min(p.x + 1, params.size.x - 1), p.y)).rgb);
    uint y0 = uint(round(a.x * 255.0)), y1 = uint(round(b.x * 255.0));
    uint u = uint(round((a.y + b.y) * 0.5 * 255.0)), v = uint(round((a.z + b.z) * 0.5 * 255.0));
    words_out[p.y * params.row_words + id.x] = u | (y0 << 8) | (v << 16) | (y1 << 24);
}

uint code10(float x) { return clamp(uint(round(x * 1023.0)), 4u, 1019u); }

// One thread per group of 6 pixels -> 4 little-endian words, the v210 layout:
//   w0 = Cb0 | Y0<<10 | Cr0<<20,  w1 = Y1 | Cb2<<10 | Y2<<20,
//   w2 = Cr2 | Y3<<10 | Cb4<<20,  w3 = Y4 | Cr4<<10 | Y5<<20
[shader("compute")]
[numthreads(16, 16, 1)]
void cs_to_v210(uint3 id : SV_DispatchThreadID)
{
    uint x0 = id.x * 6;
    if (x0 >= params.size.x || id.y >= params.size.y) return;
    float3 c[6];
    for (uint k = 0; k < 6; k++)
        c[k] = rgb_to_ycbcr(load_display(uint2(min(x0 + k, params.size.x - 1), id.y)).rgb);
    // Limited-range 10-bit: the 8-bit normalized values map onto 0..1023 with 64..940 legal.
    uint y[6];
    for (uint k = 0; k < 6; k++) y[k] = code10((c[k].x * 255.0 * 4.0) / 1023.0);
    uint cb[3], cr[3];
    for (uint k = 0; k < 3; k++)
    {
        cb[k] = code10(((c[2 * k].y + c[2 * k + 1].y) * 0.5 * 255.0 * 4.0) / 1023.0);
        cr[k] = code10(((c[2 * k].z + c[2 * k + 1].z) * 0.5 * 255.0 * 4.0) / 1023.0);
    }
    uint base = id.y * params.row_words + id.x * 4;
    words_out[base + 0] = cb[0] | (y[0] << 10) | (cr[0] << 20);
    words_out[base + 1] = y[1] | (cb[1] << 10) | (y[2] << 20);
    words_out[base + 2] = cr[1] | (y[3] << 10) | (cb[2] << 20);
    words_out[base + 3] = y[4] | (cr[2] << 10) | (y[5] << 20);
}
