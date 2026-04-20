// NisMain.hlsl
// NVScaler compute shader entry point.
// Declares all resources expected by NIS_Scaler.h and invokes NVScaler().
//
// Root signature (set by NisUpscaler):
//   Param 0: Inline CBV  b0  – NISConfig constant buffer
//   Param 1: SRV table   t0/t1/t2 – in_texture, coef_scaler, coef_usm
//   Param 2: UAV table   u0  – out_texture
//   Static sampler       s0  – linear clamp
//
// Dispatch: ceil(dstW/NIS_BLOCK_WIDTH) x ceil(dstH/NIS_BLOCK_HEIGHT) x 1

#define NIS_HLSL      1
#define NIS_SCALER    1
#define NIS_HDR_MODE  0
// Use default block size (32x24) and thread group size (256).
// NIS_BLOCK_WIDTH  is defaulted to 32 inside NIS_Scaler.h.
// NIS_BLOCK_HEIGHT is defaulted to 24 inside NIS_Scaler.h.
// NIS_THREAD_GROUP_SIZE is defaulted to 256 inside NIS_Scaler.h.

// ── NISConfig constant buffer ──────────────────────────────────────────────
// Must match the NISConfig C++ struct layout (NIS_Config.h).
cbuffer cb : register(b0)
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;

    float kContrastBoost;
    float kEps;
    float kSharpStartY;
    float kSharpScaleY;

    float kSharpStrengthMin;
    float kSharpStrengthScale;
    float kSharpLimitMin;
    float kSharpLimitScale;

    float kScaleX;
    float kScaleY;
    float kDstNormX;
    float kDstNormY;

    float kSrcNormX;
    float kSrcNormY;

    uint kInputViewportOriginX;
    uint kInputViewportOriginY;
    uint kInputViewportWidth;
    uint kInputViewportHeight;

    uint kOutputViewportOriginX;
    uint kOutputViewportOriginY;
    uint kOutputViewportWidth;
    uint kOutputViewportHeight;

    float reserved0;
    float reserved1;
};

// ── Resources ──────────────────────────────────────────────────────────────
SamplerState         samplerLinearClamp : register(s0);
Texture2D            in_texture         : register(t0);
Texture2D            coef_scaler        : register(t1);
Texture2D            coef_usm           : register(t2);
RWTexture2D<float4>  out_texture        : register(u0);

#include "NIS_Scaler.h"

// ── Entry point ────────────────────────────────────────────────────────────
[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    NVScaler(blockIdx.xy, threadIdx.x);
}
