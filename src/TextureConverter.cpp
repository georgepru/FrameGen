// TextureConverter.cpp
#include "TextureConverter.h"
#include "BGRAtoNCHW.h"    // compiled shader header (fxc-generated)
#include <stdexcept>
#include <cstring>

// ---------------------------------------------------------------------------
TextureConverter::TextureConverter(const D3DContext& ctx)
    : ctx_(ctx)
{
    BuildPipeline();
}

// ---------------------------------------------------------------------------
void TextureConverter::BuildPipeline()
{
    // ── Root signature: table (SRV t0) + table (UAV u0) + CBV b0 ──────────
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    // [0] SRV table
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[0].ShaderVisibility                     = D3D12_SHADER_VISIBILITY_ALL;
    // [1] UAV table
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[1].ShaderVisibility                     = D3D12_SHADER_VISIBILITY_ALL;
    // [2] CBV (inline root constant)
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].Descriptor.RegisterSpace  = 0;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 3;
    rsDesc.pParameters       = params;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HR_CHECK(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &rsBlob, &rsErr));
    HR_CHECK(ctx_.device12->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&rootSig_)));

    // ── Compute PSO ──────────────────────────────────────────────────────────
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig_.Get();
    psoDesc.CS             = { g_BGRAtoNCHW, sizeof(g_BGRAtoNCHW) };
    HR_CHECK(ctx_.device12->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso_)));

    // ── Descriptor heap (SRV + UAV, 2 descriptors) ──────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(ctx_.device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)));
    srvDescSize_ = ctx_.device12->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ── Constant buffer (16 bytes: Width, Height, Stride, _pad) ─────────────
    cbBuf_ = ctx_.CreateBuffer(256,
                               D3D12_RESOURCE_FLAG_NONE,
                               D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    cbBuf_->Map(0, nullptr, &cbMapped_);

    // ── Command infra ────────────────────────────────────────────────────────
    HR_CHECK(ctx_.device12->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_)));
    HR_CHECK(ctx_.device12->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_.Get(), pso_.Get(),
        IID_PPV_ARGS(&cmdList_)));
    cmdList_->Close();
}

// ---------------------------------------------------------------------------
void TextureConverter::BGRAtoNCHW(ID3D12Resource* bgraIn,
                                   ID3D12Resource* nchwOut,
                                   UINT width, UINT height,
                                   UINT paddedW, UINT paddedH,
                                   D3DContext::FenceSync& fence)
{
    // Update constant buffer
    struct CB { UINT Width, Height, Stride, _pad; } cb;
    cb.Width   = width;
    cb.Height  = height;
    cb.Stride  = paddedW;
    cb._pad    = 0;
    memcpy(cbMapped_, &cb, sizeof(cb));

    // ── Descriptors ─────────────────────────────────────────────────────────
    auto cpuBase = srvHeap_->GetCPUDescriptorHandleForHeapStart();

    // SRV: BGRA texture (descriptor 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                        = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels           = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = cpuBase;
        ctx_.device12->CreateShaderResourceView(bgraIn, &srvDesc, srvHandle);
    }

    // UAV: NCHW buffer (descriptor 1)
    {
        UINT64 bufSizeBytes = (UINT64)paddedW * paddedH * 3 * sizeof(float);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format               = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension        = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements   = (UINT)(bufSizeBytes / sizeof(float));
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuBase;
        uavHandle.ptr += srvDescSize_;
        ctx_.device12->CreateUnorderedAccessView(nchwOut, nullptr, &uavDesc, uavHandle);
    }

    // ── Record commands ──────────────────────────────────────────────────────
    HR_CHECK(cmdAlloc_->Reset());
    HR_CHECK(cmdList_->Reset(cmdAlloc_.Get(), pso_.Get()));

    // Transition bgraIn to SRV-readable
    {
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource   = bgraIn;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &bar);
    }

    cmdList_->SetComputeRootSignature(rootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetComputeRootDescriptorTable(
        0, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    auto gpuUavHandle = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    gpuUavHandle.ptr += srvDescSize_;
    cmdList_->SetComputeRootDescriptorTable(1, gpuUavHandle);
    cmdList_->SetComputeRootConstantBufferView(2, cbBuf_->GetGPUVirtualAddress());

    UINT groupsX = (width  + 7) / 8;
    UINT groupsY = (height + 7) / 8;
    cmdList_->Dispatch(groupsX, groupsY, 1);

    // Transition bgraIn back to COMMON so caller can return it to D3D11On12.
    {
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource   = bgraIn;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &bar);
    }

    HR_CHECK(cmdList_->Close());

    ID3D12CommandList* lists[] = { cmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    fence.Signal(ctx_.cmdQueue12.Get());
}

// ---------------------------------------------------------------------------
void TextureConverter::CopyBuffer(ID3D12Resource* src, ID3D12Resource* dst,
                                   UINT paddedW, UINT paddedH,
                                   D3DContext::FenceSync& fence)
{
    UINT64 bytes = (UINT64)paddedW * paddedH * 3 * sizeof(float);

    HR_CHECK(cmdAlloc_->Reset());
    HR_CHECK(cmdList_->Reset(cmdAlloc_.Get(), pso_.Get()));

    cmdList_->CopyBufferRegion(dst, 0, src, 0, bytes);

    HR_CHECK(cmdList_->Close());
    ID3D12CommandList* lists[] = { cmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    fence.Signal(ctx_.cmdQueue12.Get());
}
