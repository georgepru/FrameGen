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

    // Convert a B8G8R8A8_UNORM texture to an NCHW float16 buffer.
    // bgraIn:      BGRA D3D12 resource (state COMMON on entry, restored on exit).
    // nchwOut:     pre-allocated buffer with UAV (3 * paddedW * paddedH fp16 values).
    // bgraRefOut:  optional D3D12 Texture2D to receive a BGRA copy (for compare mode).
    //              Must be COMMON on entry; left in COMMON on exit.
    // fence:       signalled on cmdQueue12 after all commands are submitted.
    void BGRAtoNCHW(ID3D12Resource* bgraIn,
                    ID3D12Resource* nchwOut,
                    ID3D12Resource* bgraRefOut,
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
