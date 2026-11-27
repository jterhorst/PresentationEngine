// Luma-matte wipe: dark matte areas reveal first.
import engine_effect;

struct Params
{
    float softness;
    int invert;
};
ConstantBuffer<Params> params;

Texture2D<float4> tex_diagonal;

[shader("fragment")]
float4 luma_wipe(float2 uv : TEXCOORD0) : SV_Target
{
    float m = luma(tex_diagonal.Sample(engine_linear_clamp, uv).rgb);
    if (params.invert != 0)
        m = 1.0 - m;
    float s = max(params.softness, 1e-4);
    // Stretch progress so the edge fully enters and exits the frame.
    float edge = engine_frame.progress * (1.0 + 2.0 * s) - s;
    float t = smoothstep(m - s, m + s, edge);
    return lerp(sample_from(uv), sample_to(uv), t);
}
