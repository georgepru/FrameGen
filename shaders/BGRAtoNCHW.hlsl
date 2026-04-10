// BGRAtoNCHW.hlsl
// Compute shader: converts a B8G8R8A8_UNORM texture to an NCHW float32 buffer.
//
// Output buffer layout (stride = paddedWidth):
//   [0 .. H*stride-1]             R plane
//   [H*stride .. 2*H*stride-1]    G plane
//   [2*H*stride .. 3*H*stride-1]  B plane
//
// Note: HLSL sample of B8G8R8A8 auto-swizzles so .r/.g/.b give R,G,B.

Texture2D<float4> inputTex  : register(t0);
RWBuffer<float>   outputBuf : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;     // actual pixel width  (<= paddedWidth)
    uint Height;    // actual pixel height (<= paddedHeight)
    uint Stride;    // buffer row stride in floats (= paddedWidth)
    uint _pad;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Width || tid.y >= Height) return;

    // When sampling a BGRA DXVA texture, HLSL gives us the red channel in .r,
    // green in .g, blue in .b regardless of the underlying byte order.
    float4 rgba = inputTex.Load(int3(tid.xy, 0));

    uint planeStride = Stride * Height;
    uint idx         = tid.y * Stride + tid.x;

    outputBuf[0u * planeStride + idx] = rgba.r;
    outputBuf[1u * planeStride + idx] = rgba.g;
    outputBuf[2u * planeStride + idx] = rgba.b;
}
