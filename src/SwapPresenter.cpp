// SwapPresenter.cpp
#include "SwapPresenter.h"
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <chrono>

#include "NCHWPresentVS.h"
#include "NCHWPresentPS.h"
#include "NCHWPresentFSRPS.h"
#include "BGRAPresentVS.h"
#include "BGRAPresentPS.h"

#pragma comment(lib, "dxguid.lib")

// ---------------------------------------------------------------------------
SwapPresenter::SwapPresenter(HWND hwnd, const D3DContext& ctx,
                             UINT paddedW, UINT paddedH,
                             bool compareMode, UINT screenW, UINT screenH,
                             bool useFsr)
    : ctx_(ctx)
    , width_ (compareMode && screenW ? screenW : paddedW)
    , height_(compareMode && screenH ? screenH : paddedH)
    , compareMode_(compareMode)
    , useFsr_(useFsr)
{
    auto* dev     = ctx_.device12.Get();
    auto* queue   = ctx_.cmdQueue12.Get();
    auto* factory = ctx_.dxgiFactory.Get();

    // ── Swap chain ──────────────────────────────────────────────────────────
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width       = width_;
    scDesc.Height      = height_;
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc  = { 1, 0 };
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = BUFFER_COUNT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> sc1;
    HR_CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &scDesc,
                                             nullptr, nullptr, &sc1));
    HR_CHECK(sc1.As(&swapChain_));
    HR_CHECK(swapChain_->SetMaximumFrameLatency(MAX_FRAME_LATENCY));
    waitObject_ = swapChain_->GetFrameLatencyWaitableObject();

    // Disable Alt+Enter so we own fullscreen management.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // ── RTV heap ────────────────────────────────────────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = BUFFER_COUNT;
    HR_CHECK(dev->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)));
    rtvDescSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // ── SRV heap (shader-visible) ───────────────────────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 4;
    srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(dev->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_)));
    srvDescSize_ = dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ── Back buffers + RTVs ─────────────────────────────────────────────────
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        HR_CHECK(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])));
        auto rtvH = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtvH.ptr += i * rtvDescSize_;
        dev->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtvH);
    }

    // ── Per-frame command infra ──────────────────────────────────────────────
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("CreateEvent failed");

    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        HR_CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&cmdAllocs_[i])));
        HR_CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&frameFences_[i])));
    }
    HR_CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    cmdAllocs_[0].Get(), nullptr,
                                    IID_PPV_ARGS(&cmdList_)));
    cmdList_->Close();

    // ── Constant buffer (512 bytes: 256 for NCHW CB + 256 for BGRA CB) ──────────
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC   rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = 512;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HR_CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbBuf_)));
    D3D12_RANGE readRange = {};
    cbBuf_->Map(0, &readRange, &cbMapped_);

    BuildPipeline();
    if (compareMode_) BuildBGRAPipeline();
}

// ---------------------------------------------------------------------------
SwapPresenter::~SwapPresenter()
{
    if (waitObject_) CloseHandle(waitObject_);
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

// ---------------------------------------------------------------------------
void SwapPresenter::BuildPipeline()
{
    auto* dev = ctx_.device12.Get();

    // Root signature: [0] CBV b0, [1] SRV table t0
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters   = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    HR_CHECK(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &blob, &err));
    HR_CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSig_)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature        = rootSig_.Get();
    psoDesc.VS                    = { g_NCHWPresentVS, sizeof(g_NCHWPresentVS) };
    psoDesc.PS                    = useFsr_
                                    ? D3D12_SHADER_BYTECODE{ g_NCHWPresentFSRPS, sizeof(g_NCHWPresentFSRPS) }
                                    : D3D12_SHADER_BYTECODE{ g_NCHWPresentPS,    sizeof(g_NCHWPresentPS) };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc            = { 1, 0 };
    HR_CHECK(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso_)));
}

// ---------------------------------------------------------------------------
void SwapPresenter::Present(ID3D12Resource* nchwBuf,
                            UINT vidW, UINT vidH,
                            UINT paddedW, UINT paddedH,
                            const char* overlayStats,
                            ID3D12Resource* bgraRef)
{
    // Wait on waitable to avoid queuing more than MAX_FRAME_LATENCY frames.
    {
        using Clock = std::chrono::steady_clock;
        auto t0 = Clock::now();
        WaitForSingleObject(waitObject_, INFINITE);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        lastWaitMs_.store(ms);
    }

    UINT frameIdx = swapChain_->GetCurrentBackBufferIndex();

    // CPU-wait for previous rendering into this back buffer to complete.
    if (frameFences_[frameIdx]->GetCompletedValue() < fenceValues_[frameIdx])
    {
        HR_CHECK(frameFences_[frameIdx]->SetEventOnCompletion(
            fenceValues_[frameIdx], fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    HR_CHECK(cmdAllocs_[frameIdx]->Reset());
    HR_CHECK(cmdList_->Reset(cmdAllocs_[frameIdx].Get(), pso_.Get()));

    // ── Aspect-ratio UV correction ──────────────────────────────────────────
    // In compare mode: video fills the left-half panel with letterbox/pillarbox.
    // In normal mode:  UV offset = (0,0), UV scale = (1,1) → fills viewport.
    float uvOffX = 0.0f, uvOffY = 0.0f, uvScaleX = 1.0f, uvScaleY = 1.0f;
    if (compareMode_)
    {
        float panelW   = float(width_) * 0.5f;
        float panelH   = float(height_);
        float vidAsp   = float(vidW) / float(vidH);
        float panAsp   = panelW / panelH;
        if (vidAsp > panAsp)  // letterbox (bars top + bottom)
        {
            uvScaleY = panAsp / vidAsp;
            uvOffY   = (1.0f - uvScaleY) * 0.5f;
        }
        else                  // pillarbox (bars left + right)
        {
            uvScaleX = vidAsp / panAsp;
            uvOffX   = (1.0f - uvScaleX) * 0.5f;
        }
    }

    // ── Update NCHW constant buffer (at cbBuf_ + offset 0) ─────────────────
    struct CB { UINT width, height, stride; float gamma, uvOffX, uvOffY, uvScaleX, uvScaleY; };
    CB cb{ vidW, vidH, paddedW, 1.0f, uvOffX, uvOffY, uvScaleX, uvScaleY };
    memcpy(cbMapped_, &cb, sizeof(cb));

    // ── Update BGRA constant buffer (at cbBuf_ + offset 256) ───────────────
    if (compareMode_ && bgraRef)
    {
        struct BGRACB { float uvOffX, uvOffY, uvScaleX, uvScaleY; };
        BGRACB bcb{ uvOffX, uvOffY, uvScaleX, uvScaleY };
        memcpy(static_cast<char*>(cbMapped_) + 256, &bcb, sizeof(bcb));
    }

    // ── SRV slot 0: NCHW buffer ─────────────────────────────────────────────
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R16_FLOAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements      = 3 * paddedW * paddedH;
    ctx_.device12->CreateShaderResourceView(
        nchwBuf, &srvDesc, srvHeap_->GetCPUDescriptorHandleForHeapStart());

    // ── SRV slot 1: BGRA reference texture (compare mode only) ─────────────
    if (compareMode_ && bgraRef)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC bsrv = {};
        bsrv.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
        bsrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        bsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bsrv.Texture2D.MipLevels    = 1;
        auto h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += srvDescSize_;  // slot 1
        ctx_.device12->CreateShaderResourceView(bgraRef, &bsrv, h);
    }

    // ── Transition back buffer → RT ─────────────────────────────────────────
    D3D12_RESOURCE_BARRIER toRT = {};
    toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource   = backBuffers_[frameIdx].Get();
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdList_->ResourceBarrier(1, &toRT);

    auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frameIdx * rtvDescSize_;

    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ── LEFT HALF (or full screen in normal mode): NCHW buffer ─────────────
    {
        UINT halfW = compareMode_ ? (width_ / 2) : width_;
        D3D12_VIEWPORT vp = { 0, 0, float(halfW), float(height_), 0, 1 };
        D3D12_RECT     sc = { 0, 0, LONG(halfW),  LONG(height_) };
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sc);
        cmdList_->SetGraphicsRootSignature(rootSig_.Get());
        cmdList_->SetPipelineState(pso_.Get());
        cmdList_->SetGraphicsRootConstantBufferView(0, cbBuf_->GetGPUVirtualAddress());
        cmdList_->SetGraphicsRootDescriptorTable(
            1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }

    // ── RIGHT HALF: original BGRA frame (compare mode only) ────────────────
    if (compareMode_ && bgraRef && bgraPSO_)
    {
        UINT halfW = width_ / 2;
        D3D12_VIEWPORT vp = { float(halfW), 0, float(width_ - halfW), float(height_), 0, 1 };
        D3D12_RECT     sc = { LONG(halfW), 0, LONG(width_), LONG(height_) };
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sc);
        cmdList_->SetGraphicsRootSignature(bgraRS_.Get());
        cmdList_->SetPipelineState(bgraPSO_.Get());
        cmdList_->SetGraphicsRootConstantBufferView(
            0, cbBuf_->GetGPUVirtualAddress() + 256);
        auto bgrasrv = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        bgrasrv.ptr += srvDescSize_;  // slot 1
        cmdList_->SetGraphicsRootDescriptorTable(1, bgrasrv);
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }

    // ── Transition back buffer → PRESENT ───────────────────────────────────
    D3D12_RESOURCE_BARRIER toPresent = {};
    toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource   = backBuffers_[frameIdx].Get();
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cmdList_->ResourceBarrier(1, &toPresent);

    HR_CHECK(cmdList_->Close());

    ID3D12CommandList* lists[] = { cmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    // Overlay: D2D text via D3D11On12 (runs after D3D12 work, before Present)
    if (overlay_ && overlayStats && overlayStats[0])
        overlay_->Draw(frameIdx, overlayStats);

    ++fenceValues_[frameIdx];
    HR_CHECK(ctx_.cmdQueue12->Signal(
        frameFences_[frameIdx].Get(), fenceValues_[frameIdx]));

    // SyncInterval=1: gates each Present() to a vsync boundary so both frames
    // in the interpolated pair are displayed for exactly one refresh.
    HR_CHECK(swapChain_->Present(1, 0));
}

// ---------------------------------------------------------------------------
// Pass-through path: blit a raw BGRA texture directly to the swap chain.
// Used when interpolation is toggled off.
// ---------------------------------------------------------------------------
void SwapPresenter::PresentBGRA(ID3D12Resource* bgraTex,
                                 UINT /*vidW*/, UINT /*vidH*/,
                                 const char* overlayStats)
{
    WaitForSingleObject(waitObject_, INFINITE);
    UINT frameIdx = swapChain_->GetCurrentBackBufferIndex();

    if (frameFences_[frameIdx]->GetCompletedValue() < fenceValues_[frameIdx])
    {
        HR_CHECK(frameFences_[frameIdx]->SetEventOnCompletion(
            fenceValues_[frameIdx], fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    HR_CHECK(cmdAllocs_[frameIdx]->Reset());
    HR_CHECK(cmdList_->Reset(cmdAllocs_[frameIdx].Get(), nullptr));

    // Transition back buffer → COPY_DEST
    D3D12_RESOURCE_BARRIER toCD = {};
    toCD.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCD.Transition.pResource   = backBuffers_[frameIdx].Get();
    toCD.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCD.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toCD.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;

    // Transition bgraTex → COPY_SOURCE
    D3D12_RESOURCE_BARRIER toCS = {};
    toCS.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCS.Transition.pResource   = bgraTex;
    toCS.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCS.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toCS.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER barriers[] = { toCD, toCS };
    cmdList_->ResourceBarrier(2, barriers);

    cmdList_->CopyResource(backBuffers_[frameIdx].Get(), bgraTex);

    // Transition both back
    D3D12_RESOURCE_BARRIER fromCD = {};
    fromCD.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    fromCD.Transition.pResource   = backBuffers_[frameIdx].Get();
    fromCD.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    fromCD.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    fromCD.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

    D3D12_RESOURCE_BARRIER fromCS = {};
    fromCS.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    fromCS.Transition.pResource   = bgraTex;
    fromCS.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    fromCS.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fromCS.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;

    D3D12_RESOURCE_BARRIER endBarriers[] = { fromCD, fromCS };
    cmdList_->ResourceBarrier(2, endBarriers);

    HR_CHECK(cmdList_->Close());
    ID3D12CommandList* lists[] = { cmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    if (overlay_ && overlayStats && overlayStats[0])
        overlay_->Draw(frameIdx, overlayStats);

    ++fenceValues_[frameIdx];
    HR_CHECK(ctx_.cmdQueue12->Signal(
        frameFences_[frameIdx].Get(), fenceValues_[frameIdx]));

    HR_CHECK(swapChain_->Present(1, 0));  // vsync
}

// ---------------------------------------------------------------------------
// BGRA texture → right-half viewport pipeline (used only in compare mode).
// ---------------------------------------------------------------------------
void SwapPresenter::BuildBGRAPipeline()
{
    auto* dev = ctx_.device12.Get();

    // Root signature: [0] CBV b0, [1] SRV table t0, static linear sampler s0
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampDesc.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampDesc.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampDesc.ShaderRegister   = 0;
    sampDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 2;
    rsDesc.pParameters       = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampDesc;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    HR_CHECK(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &blob, &err));
    HR_CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&bgraRS_)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature           = bgraRS_.Get();
    psoDesc.VS                       = { g_BGRAPresentVS, sizeof(g_BGRAPresentVS) };
    psoDesc.PS                       = { g_BGRAPresentPS, sizeof(g_BGRAPresentPS) };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask               = UINT_MAX;
    psoDesc.PrimitiveTopologyType    = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets         = 1;
    psoDesc.RTVFormats[0]            = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc               = { 1, 0 };
    HR_CHECK(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&bgraPSO_)));
}
