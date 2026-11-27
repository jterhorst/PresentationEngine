// Three-pass glow: half-res separable blur, then add a tinted copy under the source.
import engine_effect;

struct Params
{
    float radius;     // px at output resolution
    float intensity;
    float4 tint;      // linear, premultiplied
};
ConstantBuffer<Params> params;

static const int TAPS = 9;

float4 blur(float2 uv, float2 dir)
{
    // radius is in output pixels; convert to UV so the pass scale doesn't matter.
    float2 step = dir * (max(params.radius, 0.0) / engine_frame.output_resolution) / float(TAPS);
    float4 sum = 0;
    float weight = 0;
    for (int i = -TAPS; i <= TAPS; i++)
    {
        float w = exp(-0.5 * float(i * i) / float(TAPS * TAPS) * 4.0);
        sum += engine_input0.Sample(engine_linear_clamp, uv + step * float(i)) * w;
        weight += w;
    }
    return sum / weight;
}

[shader("fragment")]
float4 blur_h(float2 uv : TEXCOORD0) : SV_Target { return blur(uv, float2(1, 0)); }

[shader("fragment")]
float4 blur_v(float2 uv : TEXCOORD0) : SV_Target { return blur(uv, float2(0, 1)); }

[shader("fragment")]
float4 combine(float2 uv : TEXCOORD0) : SV_Target
{
    float4 src = engine_input0.Sample(engine_linear_clamp, uv);
    float4 glow = engine_input1.Sample(engine_linear_clamp, uv);
    float4 tinted = glow.a * params.tint * params.intensity;
    // Premultiplied "over": source on top of the glow.
    return src + tinted * (1.0 - src.a);
}
