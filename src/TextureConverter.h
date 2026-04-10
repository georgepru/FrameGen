// TextureConverter.h
// Converts BGRA D3D12 textures → NCHW float32 buffers via a compute shader.
// Runs on the shared D3D12 direct command queue.
#pragma once
#include "Common.h"
#include "D3DContext.h"

class TextureConverter
{
public:
    explicit TextureConverter(const D3DContext& ctx);
    ~TextureConverter() = default;

    // Convert a B8G8R8A8_UNORM texture to an NCHW float32 buffer.
    // bgraIn:   BGRA D3D12 resource (at least D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).
    // nchwOut:  pre-allocated buffer with UAV (3 * paddedW * paddedH floats).
    // fence:    signalled on cmdQueue12 after all commands are submitted.
    void BGRAtoNCHW(ID3D12Resource* bgraIn,
                    ID3D12Resource* nchwOut,
                    UINT width, UINT height,
                    UINT paddedW, UINT paddedH,
                    D3DContext::FenceSync& fence);

    // GPU–GPU buffer copy on cmdQueue12.
    void CopyBuffer(ID3D12Resource* src, ID3D12Resource* dst,
                    UINT paddedW, UINT paddedH,
                    D3DContext::FenceSync& fence);

private:
    void BuildPipeline();

    const D3DContext& ctx_;

    ComPtr<ID3D12RootSignature>       rootSig_;
    ComPtr<ID3D12PipelineState>       pso_;
    ComPtr<ID3D12DescriptorHeap>      srvHeap_;
    UINT                              srvDescSize_ = 0;
    ComPtr<ID3D12Resource>            cbBuf_;
    void*                             cbMapped_ = nullptr;
    ComPtr<ID3D12CommandAllocator>    cmdAlloc_;
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
};
