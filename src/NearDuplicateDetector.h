#pragma once
#include "Common.h"
#include "D3DContext.h"

class NearDuplicateDetector
{
public:
    explicit NearDuplicateDetector(const D3DContext& ctx);

    // Returns true if the two frames are near-duplicates using a reduced GPU grid.
    bool IsNearDuplicate(ID3D12Resource* prevTex,
                         ID3D12Resource* currTex,
                         UINT width,
                         UINT height,
                         UINT changedPixelThreshold,
                         UINT* outChangedPixels = nullptr);

private:
    void BuildPipeline();

    const D3DContext& ctx_;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;

    ComPtr<ID3D12DescriptorHeap> descHeap_;
    UINT descSize_ = 0;

    ComPtr<ID3D12Resource> counterBuf_;
    ComPtr<ID3D12Resource> counterReadback_;
    ComPtr<ID3D12Resource> zeroUpload_;

    ComPtr<ID3D12CommandAllocator> cmdAlloc_;
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    D3DContext::FenceSync sync_;

    static constexpr UINT kGridW = 256;
    static constexpr UINT kGridH = 144;
    static constexpr float kColorDiffThreshold = 0.015f;
};
