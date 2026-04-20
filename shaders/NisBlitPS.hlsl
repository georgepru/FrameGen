// NisBlitPS.hlsl
// Simple fullscreen-triangle blit: samples NIS output Texture2D → backbuffer.
// Reuses the NCHWPresent VS (VSMain in NCHWPresent.hlsl) which emits UVs.
//
// Root signature (set by NisUpscaler):
//   Param 0: SRV table  t0  – NIS output texture
//   Static sampler      s0  – linear clamp

Texture2D<float4> tex : register(t0);
SamplerState      smp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(VSOut i) : SV_Target
{
    return tex.SampleLevel(smp, i.uv, 0);
}
