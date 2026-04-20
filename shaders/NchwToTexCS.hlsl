// NchwToTexCS.hlsl
// Converts an NCHW planar R16F buffer to a Texture2D RGBA16F.
//
// Root constants (b0): vidW, vidH, stride, planeSize
//   vidW      – valid video width
//   vidH      – valid video height
//   stride    – padded row stride (NCHW buffer bytes-per-row / 2)
//   planeSize – stride * paddedH  (offset between R/G/B planes)
//
// t0  Buffer<min16float>   nchwBuf  (NCHW planar R16F)
// u0  RWTexture2D<float4>  outTex   (RGBA16F, sized paddedW x paddedH)
//
// Dispatch: ceil(paddedW/8) x ceil(paddedH/8) x 1

cbuffer CB : register(b0)
{
    uint vidW;
    uint vidH;
    uint stride;
    uint planeSize;   // = stride * paddedH
}

Buffer<min16float>   nchwBuf : register(t0);
RWTexture2D<float4> outTex  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint texW, texH;
    outTex.GetDimensions(texW, texH);
    if (tid.x >= texW || tid.y >= texH)
        return;

    // Padding rows/cols: write black so NIS doesn't read uninitialized data.
    if (tid.x >= vidW || tid.y >= vidH)
    {
        outTex[tid.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    uint base = tid.y * stride + tid.x;
    float r = (float)nchwBuf[base];
    float g = (float)nchwBuf[base + planeSize];
    float b = (float)nchwBuf[base + 2u * planeSize];
    outTex[tid.xy] = float4(r, g, b, 1.0f);
}
