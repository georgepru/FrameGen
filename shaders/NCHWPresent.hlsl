// NCHWPresent.hlsl
// VS + PS pair that blits an NCHW float32 buffer (RIFE output) to the
// DXGI swap-chain back buffer.
//
// The vertex shader emits a fullscreen triangle from SV_VertexID (no VB).
// The pixel shader reads the three channel planes and outputs linear RGBA.
//
// Cbuffer layout must match the CB struct in SwapPresenter.cpp.

Buffer<min16float> nchwBuf : register(t0);

cbuffer NCHWPresentCB : register(b0)
{
    uint  Width;      // rendered viewport width (unpadded video width)
    uint  Height;     // rendered viewport height
    uint  Stride;     // buffer row stride in floats (= paddedWidth)
    float Gamma;      // 1.0 = linear pass-through, 2.2 = sRGB approx
    float UVOffsetX;  // letterbox/pillarbox correction (0 in fullscreen mode)
    float UVOffsetY;
    float UVScaleX;   // fraction of viewport covered by video (1 in fullscreen mode)
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
    float2 videoUV = (i.uv - float2(UVOffsetX, UVOffsetY))
                   / float2(UVScaleX, UVScaleY);
    if (any(videoUV < 0.0f) || any(videoUV > 1.0f))
        return float4(0, 0, 0, 1);

    uint px = (uint)(videoUV.x * Width);
    uint py = (uint)(videoUV.y * Height);
    px = min(px, Width  - 1u);
    py = min(py, Height - 1u);

    uint planeSize = Stride * Height;
    uint idx       = py * Stride + px;

    float r = nchwBuf[0u * planeSize + idx];
    float g = nchwBuf[1u * planeSize + idx];
    float b = nchwBuf[2u * planeSize + idx];

    if (Gamma != 1.0f)
    {
        r = pow(saturate(r), 1.0f / Gamma);
        g = pow(saturate(g), 1.0f / Gamma);
        b = pow(saturate(b), 1.0f / Gamma);
    }

    return float4(r, g, b, 1.0f);
}
