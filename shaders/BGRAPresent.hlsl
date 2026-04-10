// BGRAPresent.hlsl
// VS + PS pair that blits a raw BGRA Texture2D (original frame) into a
// viewport, preserving aspect ratio via UV offset/scale from the cbuffer.
//
// Used for the right half in --compare mode.
// Cbuffer layout must match the BGRACB struct in SwapPresenter.cpp.

Texture2D<float4> bgraTex : register(t0);
SamplerState      samp    : register(s0);

cbuffer BGRAPresentCB : register(b0)
{
    float UVOffsetX;  // letterbox/pillarbox offset within viewport [0,1]
    float UVOffsetY;
    float UVScaleX;   // fraction of viewport that contains the video
    float UVScaleY;
};

// ── Vertex shader ──────────────────────────────────────────────────────────

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id & 1u) * 2u, (id >> 1u) * 2u);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

// ── Pixel shader ───────────────────────────────────────────────────────────

float4 PSMain(VSOut i) : SV_Target
{
    // Map viewport UV [0,1] → video UV [0,1], applying letterbox/pillarbox
    float2 texUV = (i.uv - float2(UVOffsetX, UVOffsetY))
                 / float2(UVScaleX, UVScaleY);

    // Outside the video region → black bar
    if (any(texUV < 0.0f) || any(texUV > 1.0f))
        return float4(0, 0, 0, 1);

    // Sample the BGRA texture; HLSL auto-swizzles BGRA→RGBA so .r/.g/.b are correct
    return float4(bgraTex.Sample(samp, texUV).rgb, 1.0f);
}
