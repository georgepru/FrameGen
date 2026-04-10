// RifeInference.h
// Wraps an ONNX Runtime session with the DirectML execution provider.
// Input/output tensors are allocated as D3D12 UAV buffers on the GPU,
// shared with the DML EP via zero-copy OrtDmlApi.
#pragma once
#include "Common.h"
#include "D3DContext.h"

class RifeInference
{
public:
    // onnxPath:     path to the exported rife.onnx.
    // ctx:          shared D3D context.
    // paddedWidth/paddedHeight: video dimensions padded to 32 (buffer stride).
    RifeInference(const std::wstring& onnxPath,
                  const D3DContext&   ctx,
                  UINT paddedWidth, UINT paddedHeight);
    ~RifeInference();

    // Run inference using the internally-owned InBuf0/InBuf1 as inputs.
    // The caller must GPU-sync all writes to InBuf0/InBuf1 before calling.
    // On return the output is in OutBuf() and DML has been CPU-synced.
    void Run();

    // Direct access to the DML-backed D3D12 resources.
    // TextureConverter writes directly here to avoid extra copies.
    ID3D12Resource* InBuf0() const { return inRes0_.Get(); }
    ID3D12Resource* InBuf1() const { return inRes1_.Get(); }
    ID3D12Resource* OutBuf() const { return outRes_.Get(); }

    UINT PaddedWidth()  const { return pw_; }
    UINT PaddedHeight() const { return ph_; }

private:
    void AllocateDmlTensors();

    const D3DContext& ctx_;
    UINT pw_, ph_;

    Ort::Env       ortEnv_{ ORT_LOGGING_LEVEL_WARNING, "FrameGenMVP" };
    Ort::Session   session_{ nullptr };
    Ort::IoBinding binding_{ nullptr };

    const OrtDmlApi* dmlApi_ = nullptr;

    Ort::Value inVal0_{ nullptr };
    Ort::Value inVal1_{ nullptr };
    Ort::Value outVal_{ nullptr };

    ComPtr<ID3D12Resource> inRes0_;
    ComPtr<ID3D12Resource> inRes1_;
    ComPtr<ID3D12Resource> outRes_;

    void* dmlAlloc0_   = nullptr;
    void* dmlAlloc1_   = nullptr;
    void* dmlAllocOut_ = nullptr;
};
