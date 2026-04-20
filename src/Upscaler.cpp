// Upscaler.cpp - Simple GPU upscaler using compute shader
#include "Upscaler.h"
#include <d3dcompiler.h>

#include "d3dx12.h"

Upscaler::Upscaler(const D3DContext& ctx) : ctx_(ctx) {
    InitShader();
}

void Upscaler::InitShader() {
    // Load/compile UpscaleCS.hlsl (should be precompiled in real build)
    ComPtr<ID3DBlob> csBlob;
    HRESULT hr = D3DReadFileToBlob(L"shaders/UpscaleCS.cso", &csBlob);
    if (FAILED(hr)) throw std::runtime_error("Failed to load UpscaleCS.cso");

    // Root signature: t0 (SRV), u0 (UAV), b0 (CB)
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsDescriptorTable(1, &CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0));
    params[1].InitAsDescriptorTable(1, &CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0));
    params[2].InitAsConstantBufferView(0);
    CD3DX12_ROOT_SIGNATURE_DESC rsDesc(3, params);
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    ctx_.device12->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig_.Get();
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    ctx_.device12->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso_));
}

void Upscaler::Upscale(ID3D12Resource* srcTex, UINT srcW, UINT srcH,
                       ID3D12Resource* dstTex, UINT dstW, UINT dstH) {
    // Create descriptor heap for SRV/UAV
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    ctx_.device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));

    // SRV for srcTex
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    ctx_.device12->CreateShaderResourceView(srcTex, &srvDesc, heap->GetCPUDescriptorHandleForHeapStart());

    // UAV for dstTex
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto uavHandle = heap->GetCPUDescriptorHandleForHeapStart();
    uavHandle.ptr += ctx_.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ctx_.device12->CreateUnorderedAccessView(dstTex, nullptr, &uavDesc, uavHandle);

    // Constant buffer
    struct CB { UINT srcW, srcH, dstW, dstH; } cb = { srcW, srcH, dstW, dstH };
    ComPtr<ID3D12Resource> cbRes;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = sizeof(CB);
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ctx_.device12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbRes));
    void* cbPtr = nullptr;
    cbRes->Map(0, nullptr, &cbPtr);
    memcpy(cbPtr, &cb, sizeof(cb));
    cbRes->Unmap(0, nullptr);

    // Command list
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ctx_.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_));
    ctx_.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_.Get(), pso_.Get(), IID_PPV_ARGS(&cmdList));

    cmdList->SetComputeRootSignature(rootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = { heap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    auto uavGpuHandle = heap->GetGPUDescriptorHandleForHeapStart();
    uavGpuHandle.ptr += ctx_.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cmdList->SetComputeRootDescriptorTable(1, uavGpuHandle);
    cmdList->SetComputeRootConstantBufferView(2, cbRes->GetGPUVirtualAddress());

    UINT groupsX = (dstW + 15) / 16;
    UINT groupsY = (dstH + 15) / 16;
    cmdList->Dispatch(groupsX, groupsY, 1);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);
    // Wait for completion (simplified, not optimal for perf)
    ComPtr<ID3D12Fence> fence;
    ctx_.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ctx_.cmdQueue12->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, evt);
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
}
