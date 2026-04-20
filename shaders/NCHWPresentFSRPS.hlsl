// NCHWPresentFSRPS.hlsl
// Pixel shader: FSR 1.0 EASU-inspired upscaling from NCHW planar float16 buffer.
// Drop-in PS replacement for NCHWPresentPS — same VS, same cbuffer layout.
//
// Algorithm: 12-tap directional Catmull-Rom filter guided by luma gradient,
// matching the core idea of AMD FidelityFX Super Resolution 1.0 EASU.
// At 1:1 scale, sub-pixel offsets are always 0 → weights collapse to nearest,
// so quality is identical to bilinear at native resolution.
//
// Based on AMD FidelityFX Super Resolution 1.0 (MIT License).

Buffer<min16float> nchwBuf : register(t0);

cbuffer NCHWPresentCB : register(b0)
{
    uint  Width;       // source video width (unpadded)
    uint  Height;      // source video height (unpadded)
    uint  Stride;      // NCHW row stride in elements (padded width)
    float Gamma;
    float UVOffsetX;
    float UVOffsetY;
    float UVScaleX;
    float UVScaleY;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Load RGB from NCHW planar buffer at clamped integer pixel (x, y).
float3 NchwLoad(int x, int y)
{
    x = clamp(x, 0, (int)Stride - 1);
    y = clamp(y, 0, (int)Height - 1);
    uint planeSize = Stride * Height;
    uint idx       = (uint)y * Stride + (uint)x;
    return float3(
        (float)nchwBuf[0u * planeSize + idx],
        (float)nchwBuf[1u * planeSize + idx],
        (float)nchwBuf[2u * planeSize + idx]);
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Catmull-Rom weights for 4 taps: t-1, t, t+1, t+2 at sub-pixel offset f.
void CRWeights(float f, out float w0, out float w1, out float w2, out float w3)
{
    float f2 = f * f, f3 = f2 * f;
    w0 = -0.5*f3 + 1.0*f2 - 0.5*f;
    w1 =  1.5*f3 - 2.5*f2 + 1.0;
    w2 = -1.5*f3 + 2.0*f2 + 0.5*f;
    w3 =  0.5*f3 - 0.5*f2;
}

// ---------------------------------------------------------------------------
// FSR 1.0 EASU core: 12-tap directional filter.
//
// 12-tap layout around integer base (p) + sub-pixel (fx, fy):
//
//      [B][C]
//  [A][E][F][G]
//  [H][I][J][K]
//      [L][M]
//
// Two partial-CR passes are computed:
//   H-pass: Catmull-Rom in X across rows 0&1, linear blend in Y.
//   V-pass: Catmull-Rom in Y across cols 0&1, linear blend in X.
// Gradient-driven blend: vertical edges → H-pass; horizontal edges → V-pass.
//
float3 FsrEasu(float2 sp)
{
    int2  p  = int2(floor(sp));
    float fx = sp.x - (float)p.x;
    float fy = sp.y - (float)p.y;

    // Fetch 12 neighbors
    float3 B = NchwLoad(p.x+0, p.y-1);
    float3 C = NchwLoad(p.x+1, p.y-1);
    float3 A = NchwLoad(p.x-1, p.y  );
    float3 E = NchwLoad(p.x+0, p.y  );
    float3 F = NchwLoad(p.x+1, p.y  );
    float3 G = NchwLoad(p.x+2, p.y  );
    float3 H = NchwLoad(p.x-1, p.y+1);
    float3 I = NchwLoad(p.x+0, p.y+1);
    float3 J = NchwLoad(p.x+1, p.y+1);
    float3 K = NchwLoad(p.x+2, p.y+1);
    float3 L = NchwLoad(p.x+0, p.y+2);
    float3 M = NchwLoad(p.x+1, p.y+2);

    // Lumas
    float lB = Luma(B), lC = Luma(C);
    float lA = Luma(A), lE = Luma(E), lF = Luma(F), lG = Luma(G);
    float lH = Luma(H), lI = Luma(I), lJ = Luma(J), lK = Luma(K);
    float lL = Luma(L), lM = Luma(M);

    // Gradient: dX strong → vertical edge → H-pass dominates
    //           dY strong → horizontal edge → V-pass dominates
    float dX = abs(lA - lE) + abs(lE - lF) + abs(lF - lG)
             + abs(lH - lI) + abs(lI - lJ) + abs(lJ - lK)
             + abs(lB - lC) + abs(lL - lM);
    float dY = abs(lB - lE) + abs(lE - lI) + abs(lI - lL)
             + abs(lC - lF) + abs(lF - lJ) + abs(lJ - lM)
             + abs(lA - lH) + abs(lG - lK);
    float pX = dX / (dX + dY + 1e-5f);   // 1 = H-pass, 0 = V-pass

    // H-pass: CR in X for rows 0 and 1, then linear blend in Y
    float wx0, wx1, wx2, wx3;
    CRWeights(fx, wx0, wx1, wx2, wx3);
    float3 hRow0 = A * wx0 + E * wx1 + F * wx2 + G * wx3;
    float3 hRow1 = H * wx0 + I * wx1 + J * wx2 + K * wx3;
    float3 hResult = lerp(hRow0, hRow1, fy);

    // V-pass: CR in Y for cols 0 and 1, then linear blend in X
    float wy0, wy1, wy2, wy3;
    CRWeights(fy, wy0, wy1, wy2, wy3);
    float3 vCol0 = B * wy0 + E * wy1 + I * wy2 + L * wy3;
    float3 vCol1 = C * wy0 + F * wy1 + J * wy2 + M * wy3;
    float3 vResult = lerp(vCol0, vCol1, fx);

    return lerp(vResult, hResult, pX);
}

// ---------------------------------------------------------------------------
float4 PSMain(VSOut i) : SV_Target
{
    float2 videoUV = (i.uv - float2(UVOffsetX, UVOffsetY))
                   / float2(UVScaleX, UVScaleY);
    if (any(videoUV < 0.0f) || any(videoUV > 1.0f))
        return float4(0, 0, 0, 1);

    float2 sp = videoUV * float2(Width, Height) - 0.5;
    float3 col = FsrEasu(sp);

    if (Gamma != 1.0f)
    {
        col.r = pow(saturate(col.r), 1.0f / Gamma);
        col.g = pow(saturate(col.g), 1.0f / Gamma);
        col.b = pow(saturate(col.b), 1.0f / Gamma);
    }

    return float4(col, 1.0f);
}
