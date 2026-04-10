// SwapPresenter.h
// Manages a DXGI flip-model swap chain with a frame-latency waitable object.
// Presents an NCHW float32 buffer (RIFE output) to a fullscreen window.
#pragma once
#include "Common.h"
#include "D3DContext.h"
#include "Overlay.h"

class SwapPresenter
{
public:
    static constexpr UINT BUFFER_COUNT     = 2;
    static constexpr UINT MAX_FRAME_LATENCY = 1;  // tightest: 1 frame queued

    SwapPresenter(HWND hwnd, const D3DContext& ctx, UINT paddedW, UINT paddedH,
                  bool compareMode = false, UINT screenW = 0, UINT screenH = 0);
    ~SwapPresenter();

    // Present the NCHW float16 output buffer to the display.
    // In compare mode, also renders bgraRef (original frame) to the right half.
    // Blocks on the waitable object to ensure we never get ahead of the display.
    void Present(ID3D12Resource* nchwBuf,
                 UINT vidW, UINT vidH,
                 UINT paddedW, UINT paddedH,
                 const char* overlayStats = nullptr,
                 ID3D12Resource* bgraRef  = nullptr);

    // Also expose a pass-through path (no RIFE — raw BGRA texture).
    // Used when interpolation is disabled.
    void PresentBGRA(ID3D12Resource* bgraTex,
                     UINT vidW, UINT vidH,
                     const char* overlayStats = nullptr);

    void SetInterpolationEnabled(bool en) { interpolationEnabled_ = en; }
    bool IsInterpolationEnabled()  const  { return interpolationEnabled_; }

    // Optional stats overlay — set once after construction.
    // The Overlay object must outlive this presenter.
    void SetOverlay(Overlay* overlay) { overlay_ = overlay; }

    IDXGISwapChain3* SwapChain() const { return swapChain_.Get(); }

private:
    void BuildPipeline();
    void BuildBGRAPipeline();

    const D3DContext& ctx_;
    UINT width_, height_;      // swapchain dimensions (full screen in compare mode)
    bool     compareMode_      = false;
    bool     interpolationEnabled_ = true;
    Overlay* overlay_              = nullptr;

    ComPtr<IDXGISwapChain3>   swapChain_;
    HANDLE                    waitObject_  = nullptr;

    ComPtr<ID3D12Resource>            backBuffers_[BUFFER_COUNT];
    ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    UINT                              rtvDescSize_ = 0;

    ComPtr<ID3D12DescriptorHeap>      srvHeap_;    // shader-visible: NCHW SRV
    UINT                              srvDescSize_ = 0;

    // NCHW present pipeline (float32 RIFE output)
    ComPtr<ID3D12RootSignature>       rootSig_;
    ComPtr<ID3D12PipelineState>       pso_;

    // BGRA pass-through pipeline
    ComPtr<ID3D12RootSignature>       bgraRS_;
    ComPtr<ID3D12PipelineState>       bgraPSO_;

    // Per-frame command infra
    ComPtr<ID3D12CommandAllocator>    cmdAllocs_[BUFFER_COUNT];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;

    // Per-frame fence for GPU/CPU sync
    ComPtr<ID3D12Fence>               frameFences_[BUFFER_COUNT];
    UINT64                            fenceValues_[BUFFER_COUNT] = {};
    HANDLE                            fenceEvent_ = nullptr;

    // Constant buffer (Width, Height, Stride, Gamma)
    ComPtr<ID3D12Resource>            cbBuf_;
    void*                             cbMapped_ = nullptr;
};
