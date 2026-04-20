// Upscaler.h - Simple GPU upscaler using compute shader
#pragma once
#include "Common.h"
#include "D3DContext.h"

class Upscaler {
public:
    Upscaler(const D3DContext& ctx);
    // Upscales srcTex (RGBA float) to dstTex (RGBA float) using compute shader
    void Upscale(ID3D12Resource* srcTex, UINT srcW, UINT srcH,
                 ID3D12Resource* dstTex, UINT dstW, UINT dstH);
private:
    const D3DContext& ctx_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12RootSignature> rootSig_;
    void InitShader();
};
