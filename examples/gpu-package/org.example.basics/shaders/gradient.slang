// Animated two-color gradient source.
import engine_effect;

struct Params
{
    float4 color_a;
    float4 color_b;
    float speed;
    int direction; // 0 = horizontal, 1 = vertical
};
ConstantBuffer<Params> params;

[shader("fragment")]
float4 gradient(float2 uv : TEXCOORD0) : SV_Target
{
    float axis = params.direction == 0 ? uv.x : uv.y;
    float t = 0.5 + 0.5 * sin((axis + engine_frame.time * params.speed) * 6.2831853);
    return lerp(params.color_a, params.color_b, t);
}
