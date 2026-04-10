// Overlay.h
// Renders a stats overlay (FPS, RIFE ms, latency) onto D3D12 swap chain back
// buffers using D2D1 + DWrite via D3D11On12 interop.
//
// Usage:
//   1. Construct after the swap chain back buffers exist.
//   2. Call Draw() between the NCHW present draw and the final
//      PRESENT barrier — back buffer must be in RENDER_TARGET state.
//   3. Call ResizeBuffers() if the swap chain is resized (not used in MVP).
#pragma once
#include "Common.h"
#include "D3DContext.h"
#include <d2d1_3.h>
#include <dwrite_3.h>

class Overlay
{
public:
    // backBuffers: the swap chain back buffers (DXGI_FORMAT_R8G8B8A8_UNORM).
    // bufferCount: number of back buffers (must match SwapPresenter::BUFFER_COUNT).
    Overlay(const D3DContext& ctx,
            IDXGISwapChain3* swapChain,
            UINT              bufferCount,
            UINT              width,
            UINT              height);
    ~Overlay() = default;

    // Draw the overlay onto the back buffer at frameIndex.
    // Call this while the back buffer is in RENDER_TARGET state,
    // BEFORE the final RT→PRESENT barrier.
    // stats: null-terminated UTF-8 string (multi-line, '\n' separated).
    void Draw(UINT frameIndex, const char* stats);

private:
    const D3DContext& ctx_;
    UINT width_, height_;
    UINT bufferCount_;

    // D2D / DWrite
    ComPtr<ID2D1Factory3>       d2dFactory_;
    ComPtr<ID2D1Device2>        d2dDevice_;
    ComPtr<ID2D1DeviceContext2> d2dCtx_;
    ComPtr<IDWriteFactory3>     dwriteFactory_;
    ComPtr<IDWriteTextFormat>   textFormat_;

    // Per-back-buffer D2D render target surfaces (wrapped via D3D11On12)
    struct Surface
    {
        ComPtr<ID3D11Resource>   wrapped11;   // D3D11On12 wrapped back buffer
        ComPtr<ID2D1Bitmap1>     bitmap;      // D2D render target on that surface
    };
    std::vector<Surface> surfaces_;

    // Brushes
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> bgBrush_;
};
