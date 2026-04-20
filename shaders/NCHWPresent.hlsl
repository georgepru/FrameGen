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

    // Bilinear sampling from NCHW buffer.
    // Works correctly at 1:1 (frac=0 → identical to nearest) and for upscaling.
    float2 coord = videoUV * float2(Width, Height) - 0.5;
    int2   c0    = int2(floor(coord));
    int2   c1    = c0 + int2(1, 1);
    float2 f     = frac(coord);

    // Clamp to valid range
    uint px0 = (uint)clamp(c0.x, 0, (int)Width  - 1);
    uint py0 = (uint)clamp(c0.y, 0, (int)Height - 1);
    uint px1 = (uint)clamp(c1.x, 0, (int)Width  - 1);
    uint py1 = (uint)clamp(c1.y, 0, (int)Height - 1);

    uint planeSize = Stride * Height;

    // 4-tap bilinear for each channel
    float r = lerp(lerp((float)nchwBuf[0u*planeSize + py0*Stride + px0],
                        (float)nchwBuf[0u*planeSize + py0*Stride + px1], f.x),
                   lerp((float)nchwBuf[0u*planeSize + py1*Stride + px0],
                        (float)nchwBuf[0u*planeSize + py1*Stride + px1], f.x), f.y);
    float g = lerp(lerp((float)nchwBuf[1u*planeSize + py0*Stride + px0],
                        (float)nchwBuf[1u*planeSize + py0*Stride + px1], f.x),
                   lerp((float)nchwBuf[1u*planeSize + py1*Stride + px0],
                        (float)nchwBuf[1u*planeSize + py1*Stride + px1], f.x), f.y);
    float b = lerp(lerp((float)nchwBuf[2u*planeSize + py0*Stride + px0],
                        (float)nchwBuf[2u*planeSize + py0*Stride + px1], f.x),
                   lerp((float)nchwBuf[2u*planeSize + py1*Stride + px0],
                        (float)nchwBuf[2u*planeSize + py1*Stride + px1], f.x), f.y);

    if (Gamma != 1.0f)
    {
        r = pow(saturate(r), 1.0f / Gamma);
        g = pow(saturate(g), 1.0f / Gamma);
        b = pow(saturate(b), 1.0f / Gamma);
    }

    return float4(r, g, b, 1.0f);
}
