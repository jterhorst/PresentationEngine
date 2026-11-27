// Shader interface for GPU packages (effects, transitions, sources).
//
// Every pass is a fragment shader over a fullscreen triangle supplied by the
// engine. Write a function with this signature and name it in package.json:
//
//     [shader("fragment")]
//     float4 my_entry(float2 uv : TEXCOORD0) : SV_Target
//
// Color contract: every texture you read and the value you return is
// LINEAR-light, PREMULTIPLIED-alpha RGBA (stored as RGBA16F). Blend modes,
// opacity and masks are applied by the engine after your pass.
//
// Parameters: declare `ConstantBuffer<Params> params;` where each field name
// equals a param id from package.json. Types map as:
//   number -> float, integer -> int, boolean -> int (0/1), color -> float4
//   (linear, premultiplied), vec2 -> float2, choice -> int (index in choices).
// The engine matches fields by name through reflection; declaration order is free.
//
// Package media listed in "textures" bind by name as `Texture2D<float4> tex_<id>;`.
// Video media advances with time and loops if shorter than the content.
module engine_effect;

public static const int ENGINE_EFFECT_API = 1;

public struct EngineFrame
{
    public float time;          // seconds since the effect/source appeared, or since the transition began
    public float progress;      // transitions: eased progress 0..1; otherwise 0
    public float duration;      // transitions: seconds; otherwise 0
    public float2 resolution;   // pixels of this pass's render target
    public float2 texel_size;   // 1 / resolution
    public float2 output_resolution; // pixels of the effect's final result (passes may run smaller)
    public float2 canvas_scale; // output pixels per canvas unit
    public uint frame_index;
    public uint _pad0;
};

// Bound by the engine for every pass.
public ConstantBuffer<EngineFrame> engine_frame;
public SamplerState engine_linear_clamp;
public SamplerState engine_linear_repeat;

// Effects and sources: inputs as declared by the pass ("source", "pass:<id>", "media:<id>").
// Single-shader effects receive the element's rendered pixels as engine_input0.
public Texture2D<float4> engine_input0;
public Texture2D<float4> engine_input1;
public Texture2D<float4> engine_input2;
public Texture2D<float4> engine_input3;

// Transitions: outgoing and incoming content, same size as the output.
public Texture2D<float4> engine_from;
public Texture2D<float4> engine_to;

// Helpers.
public float4 sample_input0(float2 uv) { return engine_input0.Sample(engine_linear_clamp, uv); }
public float4 sample_from(float2 uv)   { return engine_from.Sample(engine_linear_clamp, uv); }
public float4 sample_to(float2 uv)     { return engine_to.Sample(engine_linear_clamp, uv); }

public float luma(float3 linear_rgb) { return dot(linear_rgb, float3(0.2126, 0.7152, 0.0722)); }
