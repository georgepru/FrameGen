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
    uint  Width;    // rendered viewport width (unpadded video width)
    uint  Height;   // rendered viewport height
    uint  Stride;   // buffer row stride in floats (= paddedWidth)
    float Gamma;    // 1.0 = linear pass-through, 2.2 = sRGB approx
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
    uint px = (uint)(i.uv.x * Width);
    uint py = (uint)(i.uv.y * Height);
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
