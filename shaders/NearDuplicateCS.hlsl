cbuffer CompareCB : register(b0)
{
    uint inputW;
    uint inputH;
    uint gridW;
    uint gridH;
    float colorDiffThreshold;
};

Texture2D<float4> g_prev : register(t0);
Texture2D<float4> g_curr : register(t1);
RWStructuredBuffer<uint> g_changedCount : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gridW || tid.y >= gridH)
        return;

    uint x = ((tid.x * inputW) + (gridW / 2)) / gridW;
    uint y = ((tid.y * inputH) + (gridH / 2)) / gridH;
    x = min(x, inputW - 1);
    y = min(y, inputH - 1);

    float3 a = g_prev.Load(int3(x, y, 0)).rgb;
    float3 b = g_curr.Load(int3(x, y, 0)).rgb;
    float3 d = abs(a - b);
    float maxDiff = max(d.r, max(d.g, d.b));

    if (maxDiff > colorDiffThreshold)
    {
        InterlockedAdd(g_changedCount[0], 1);
    }
}
