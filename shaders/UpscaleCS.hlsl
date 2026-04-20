// UpscaleCS.hlsl - Simple bilinear upscaling compute shader
// Input: srcTex (float4 RGBA), Output: dstTex (float4 RGBA)
// Dispatch: ceil(dstWidth/16), ceil(dstHeight/16), 1

Texture2D<float4> srcTex : register(t0);
RWTexture2D<float4> dstTex : register(u0);
cbuffer UpscaleCB : register(b0)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
}

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= dstWidth || DTid.y >= dstHeight) return;
    float2 uv = float2(DTid.x + 0.5, DTid.y + 0.5) / float2(dstWidth, dstHeight);
    float2 srcCoord = uv * float2(srcWidth, srcHeight) - 0.5;
    float2 srcPix = floor(srcCoord);
    float2 f = srcCoord - srcPix;
    float4 c00 = srcTex.Load(int3(srcPix, 0));
    float4 c10 = srcTex.Load(int3(srcPix + float2(1,0), 0));
    float4 c01 = srcTex.Load(int3(srcPix + float2(0,1), 0));
    float4 c11 = srcTex.Load(int3(srcPix + float2(1,1), 0));
    float4 c0 = lerp(c00, c10, f.x);
    float4 c1 = lerp(c01, c11, f.x);
    float4 c = lerp(c0, c1, f.y);
    dstTex[DTid.xy] = c;
}
