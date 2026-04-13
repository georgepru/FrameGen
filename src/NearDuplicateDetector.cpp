#include "NearDuplicateDetector.h"
#include "NearDuplicateCS.h"

NearDuplicateDetector::NearDuplicateDetector(const D3DContext& ctx)
    : ctx_(ctx), sync_(ctx_.CreateFenceSync())
{
    auto* dev = ctx_.device12.Get();

    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh.NumDescriptors = 3;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&descHeap_)));
    descSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    counterBuf_ = ctx_.CreateBuffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    counterReadback_ = ctx_.CreateBuffer(sizeof(UINT), D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    zeroUpload_ = ctx_.CreateBuffer(sizeof(UINT), D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    {
        UINT* p = nullptr;
        D3D12_RANGE r = {};
        HR_CHECK(zeroUpload_->Map(0, &r, reinterpret_cast<void**>(&p)));
        *p = 0;
        zeroUpload_->Unmap(0, nullptr);
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1;

    auto uavCpu = descHeap_->GetCPUDescriptorHandleForHeapStart();
    uavCpu.ptr += static_cast<SIZE_T>(2) * descSize_;
    dev->CreateUnorderedAccessView(counterBuf_.Get(), nullptr, &uav, uavCpu);

    HR_CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_)));
    HR_CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_.Get(), nullptr,
                                    IID_PPV_ARGS(&cmdList_)));
    cmdList_->Close();

    BuildPipeline();
}

void NearDuplicateDetector::BuildPipeline()
{
    auto* dev = ctx_.device12.Get();

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2;
    srvRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 5;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;

    ComPtr<ID3DBlob> blob, err;
    HR_CHECK(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    HR_CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSig_)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rootSig_.Get();
    pso.CS = { g_NearDuplicateCS, sizeof(g_NearDuplicateCS) };
    HR_CHECK(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)));
}

bool NearDuplicateDetector::IsNearDuplicate(ID3D12Resource* prevTex,
                                            ID3D12Resource* currTex,
                                            UINT width,
                                            UINT height,
                                            UINT changedPixelThreshold,
                                            UINT* outChangedPixels)
{
    if (!prevTex || !currTex || width == 0 || height == 0) return false;

    auto* dev = ctx_.device12.Get();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    auto baseCpu = descHeap_->GetCPUDescriptorHandleForHeapStart();
    auto srv0 = baseCpu;
    auto srv1 = baseCpu;
    srv1.ptr += descSize_;
    dev->CreateShaderResourceView(prevTex, &srv, srv0);
    dev->CreateShaderResourceView(currTex, &srv, srv1);

    HR_CHECK(cmdAlloc_->Reset());
    HR_CHECK(cmdList_->Reset(cmdAlloc_.Get(), pso_.Get()));

    D3D12_RESOURCE_BARRIER toUse[3] = {};
    toUse[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUse[0].Transition.pResource = prevTex;
    toUse[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUse[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toUse[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    toUse[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUse[1].Transition.pResource = currTex;
    toUse[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUse[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toUse[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    toUse[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUse[2].Transition.pResource = counterBuf_.Get();
    toUse[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUse[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toUse[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    cmdList_->ResourceBarrier(3, toUse);
    cmdList_->CopyBufferRegion(counterBuf_.Get(), 0, zeroUpload_.Get(), 0, sizeof(UINT));

    D3D12_RESOURCE_BARRIER counterToUav = {};
    counterToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    counterToUav.Transition.pResource = counterBuf_.Get();
    counterToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    counterToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    counterToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList_->ResourceBarrier(1, &counterToUav);

    ID3D12DescriptorHeap* heaps[] = { descHeap_.Get() };
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetComputeRootSignature(rootSig_.Get());

    auto baseGpu = descHeap_->GetGPUDescriptorHandleForHeapStart();
    auto srvGpu = baseGpu;
    auto uavGpu = baseGpu;
    uavGpu.ptr += static_cast<SIZE_T>(2) * descSize_;

    cmdList_->SetComputeRootDescriptorTable(0, srvGpu);
    cmdList_->SetComputeRootDescriptorTable(1, uavGpu);

    float colorThresh = kColorDiffThreshold;
    UINT c[5] = { width, height, kGridW, kGridH, *reinterpret_cast<UINT*>(&colorThresh) };
    cmdList_->SetComputeRoot32BitConstants(2, 5, c, 0);

    const UINT gx = (kGridW + 7) / 8;
    const UINT gy = (kGridH + 7) / 8;
    cmdList_->Dispatch(gx, gy, 1);

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = counterBuf_.Get();
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList_->ResourceBarrier(1, &toCopy);

    cmdList_->CopyBufferRegion(counterReadback_.Get(), 0, counterBuf_.Get(), 0, sizeof(UINT));

    D3D12_RESOURCE_BARRIER toCommon[3] = {};
    toCommon[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCommon[0].Transition.pResource = prevTex;
    toCommon[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCommon[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCommon[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

    toCommon[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCommon[1].Transition.pResource = currTex;
    toCommon[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCommon[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCommon[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

    toCommon[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCommon[2].Transition.pResource = counterBuf_.Get();
    toCommon[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCommon[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCommon[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList_->ResourceBarrier(3, toCommon);

    HR_CHECK(cmdList_->Close());
    ID3D12CommandList* lists[] = { cmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    sync_.Signal(ctx_.cmdQueue12.Get());
    sync_.Wait();

    UINT changed = 0;
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, sizeof(UINT) };
    HR_CHECK(counterReadback_->Map(0, &readRange, &mapped));
    changed = *reinterpret_cast<UINT*>(mapped);
    counterReadback_->Unmap(0, nullptr);

    if (outChangedPixels) *outChangedPixels = changed;
    return changed <= changedPixelThreshold;
}
