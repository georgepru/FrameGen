// D3DContext.cpp
#include "D3DContext.h"
#include <stdexcept>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------
D3DContext D3DContext::Create(bool enableDebug)
{
    D3DContext ctx;

    // 1. Debug layer ---------------------------------------------------------
    if (enableDebug)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }

    // 2. DXGI factory --------------------------------------------------------
    UINT factoryFlags = enableDebug ? DXGI_CREATE_FACTORY_DEBUG : 0u;
    HR_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&ctx.dxgiFactory)));

    // 3. Pick highest-perf hardware adapter ----------------------------------
    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (ctx.dxgiFactory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                                        D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr)))
        {
            ctx.adapter = adapter;
            break;
        }
    }
    if (!ctx.adapter)
        throw std::runtime_error("No D3D12 hardware adapter found");

    // 4. D3D12 device --------------------------------------------------------
    HR_CHECK(D3D12CreateDevice(ctx.adapter.Get(),
                               D3D_FEATURE_LEVEL_12_0,
                               IID_PPV_ARGS(&ctx.device12)));

    // 5. Command queues ------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HR_CHECK(ctx.device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&ctx.cmdQueue12)));

    // Dedicated queue for ORT/DML to avoid D3D11On12 fence interference.
    HR_CHECK(ctx.device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&ctx.dmlQueue12)));

    // 6. D3D11On12 -----------------------------------------------------------
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    UINT d3d11Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (enableDebug) d3d11Flags |= D3D11_CREATE_DEVICE_DEBUG;

    IUnknown* queues[] = { ctx.cmdQueue12.Get() };
    HR_CHECK(D3D11On12CreateDevice(
        ctx.device12.Get(), d3d11Flags,
        &fl, 1, queues, 1, 0,
        &ctx.device11, &ctx.ctx11, nullptr));
    HR_CHECK(ctx.device11.As(&ctx.on12));

    // 7. DirectML ------------------------------------------------------------
    typedef HRESULT (WINAPI* PFN_DMLCreate)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    HMODULE hDML = LoadLibraryW(L"DirectML.dll");
    if (!hDML) throw std::runtime_error("DirectML.dll not found");
    auto pfnDML = reinterpret_cast<PFN_DMLCreate>(GetProcAddress(hDML, "DMLCreateDevice"));
    if (!pfnDML) throw std::runtime_error("DMLCreateDevice not found");
    DML_CREATE_DEVICE_FLAGS dmlFlags =
        enableDebug ? DML_CREATE_DEVICE_FLAG_DEBUG : DML_CREATE_DEVICE_FLAG_NONE;
    HR_CHECK(pfnDML(ctx.device12.Get(), dmlFlags, IID_PPV_ARGS(&ctx.dmlDevice)));

    // 8. InfoQueue -----------------------------------------------------------
    ctx.device12.As(&ctx.infoQueue_);
    if (ctx.infoQueue_)
        ctx.infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);

    return ctx;
}

// ---------------------------------------------------------------------------
void D3DContext::FlushQueue() const
{
    auto sync = CreateFenceSync();
    sync.Signal(cmdQueue12.Get());
    sync.Wait();
}

void D3DContext::FlushDmlQueue() const
{
    auto sync = CreateFenceSync();
    sync.Signal(dmlQueue12.Get());
    sync.Wait();
}

// ---------------------------------------------------------------------------
D3DContext::FenceSync D3DContext::CreateFenceSync() const
{
    FenceSync s;
    HR_CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event) throw std::runtime_error("CreateEvent failed");
    return s;
}

void D3DContext::FenceSync::Signal(ID3D12CommandQueue* queue)
{
    ++value;
    HR_CHECK(queue->Signal(fence.Get(), value));
}

void D3DContext::FenceSync::Wait()
{
    if (fence->GetCompletedValue() < value)
    {
        HR_CHECK(fence->SetEventOnCompletion(value, event));
        WaitForSingleObject(event, INFINITE);
    }
}

// ---------------------------------------------------------------------------
ComPtr<ID3D12Resource> D3DContext::CreateBuffer(
    UINT64                sizeBytes,
    D3D12_RESOURCE_FLAGS  flags,
    D3D12_HEAP_TYPE       heapType,
    D3D12_RESOURCE_STATES initState) const
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heapType;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = sizeBytes;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc         = {1, 0};
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags              = flags;

    ComPtr<ID3D12Resource> res;
    HR_CHECK(device12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

// ---------------------------------------------------------------------------
ComPtr<ID3D12Resource> D3DContext::CreateTexture2D(
    UINT                  width,
    UINT                  height,
    DXGI_FORMAT           format,
    D3D12_RESOURCE_FLAGS  flags,
    D3D12_RESOURCE_STATES initState) const
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width              = width;
    rd.Height             = height;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = format;
    rd.SampleDesc         = {1, 0};
    rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags              = flags;

    ComPtr<ID3D12Resource> res;
    HR_CHECK(device12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

// ---------------------------------------------------------------------------
void D3DContext::DumpDebugMessages() const
{
    if (!infoQueue_) return;
    // ID3D12InfoQueue::GetMessage conflicts with the Win32 GetMessage macro
    // (winuser.h defines GetMessage→GetMessageA before d3d12sdklayers.h is
    // processed, renaming the vtable slot).  Print the count only; detailed
    // messages are visible in the VS Output window or PIX when the debug
    // layer is enabled.
    UINT64 n = infoQueue_->GetNumStoredMessages();
    if (n > 0)
    {
        printf("[D3D12] %llu validation message(s) — enable VS GPU debug output for details\n",
               static_cast<unsigned long long>(n));
        fflush(stdout);
    }
    infoQueue_->ClearStoredMessages();
}
