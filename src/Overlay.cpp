// Overlay.cpp
#include "Overlay.h"
#include <stdexcept>
#include <cstdio>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ---------------------------------------------------------------------------
Overlay::Overlay(const D3DContext& ctx,
                 IDXGISwapChain3* swapChain,
                 UINT              bufferCount,
                 UINT              width,
                 UINT              height)
    : ctx_(ctx), bufferCount_(bufferCount), width_(width), height_(height)
{
    surfaces_.resize(bufferCount);

    // ── D2D factory ─────────────────────────────────────────────────────────
    D2D1_FACTORY_OPTIONS opts = {};
    HR_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory3), &opts,
                               reinterpret_cast<void**>(d2dFactory_.GetAddressOf())));

    // ── D2D device from the D3D11 device (which is backed by D3D12 via On12) ─
    ComPtr<IDXGIDevice> dxgiDevice;
    HR_CHECK(ctx_.device11.As(&dxgiDevice));
    ComPtr<ID2D1Device2> d2dDev;
    HR_CHECK(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_));
    HR_CHECK(d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_));

    // ── DWrite factory + text format ────────────────────────────────────────
    HR_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory3),
                                 reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())));

    HR_CHECK(dwriteFactory_->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        20.0f,          // font size in DIPs
        L"en-us",
        &textFormat_));
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // ── Per-back-buffer D2D render target ────────────────────────────────────
    // D2D needs a DXGI surface backed by a D3D11 resource.
    // We create a D3D11On12 wrapped resource for each back buffer, then
    // get its DXGI surface, then create a D2D bitmap pointing to it.
    float dpi = 96.0f;  // assume 100% scaling; text coords are in pixels
    D2D1_BITMAP_PROPERTIES1 bmpProps = {};
    bmpProps.pixelFormat = D2D1::PixelFormat(
        DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    bmpProps.dpiX         = dpi;
    bmpProps.dpiY         = dpi;
    bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    for (UINT i = 0; i < bufferCount; ++i)
    {
        ComPtr<ID3D12Resource> bb12;
        HR_CHECK(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb12)));

        D3D11_RESOURCE_FLAGS d3d11Flags = {};
        d3d11Flags.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Resource> res11;
        HR_CHECK(ctx_.on12->CreateWrappedResource(
            bb12.Get(),                            // IUnknown* – the D3D12 back buffer
            &d3d11Flags,
            D3D12_RESOURCE_STATE_PRESENT,          // InState  (back buffer after D3D12 rendering)
            D3D12_RESOURCE_STATE_PRESENT,          // OutState (back buffer before swapChain->Present)
            IID_PPV_ARGS(&res11)));
        surfaces_[i].wrapped11 = res11;

        ComPtr<IDXGISurface> dxgiSurf;
        HR_CHECK(surfaces_[i].wrapped11.As(&dxgiSurf));

        HR_CHECK(d2dCtx_->CreateBitmapFromDxgiSurface(
            dxgiSurf.Get(), &bmpProps, &surfaces_[i].bitmap));
    }

    // ── Brushes ─────────────────────────────────────────────────────────────
    HR_CHECK(d2dCtx_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &textBrush_));   // white
    HR_CHECK(d2dCtx_->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &bgBrush_));    // semi-transparent black
}

// ---------------------------------------------------------------------------
void Overlay::Draw(UINT frameIndex, const char* stats)
{
    // Convert UTF-8 stats to wide string for DWrite.
    // stats is a short ASCII string so simple cast is fine.
    int len = static_cast<int>(strlen(stats));
    std::wstring wide(len, L'\0');
    for (int i = 0; i < len; ++i)
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(stats[i]));

    // Acquire the D3D11On12 wrapped surface (transitions D3D12 → D3D11).
    ID3D11Resource* res11s[] = { surfaces_[frameIndex].wrapped11.Get() };
    ctx_.on12->AcquireWrappedResources(res11s, 1);

    // D2D draw
    d2dCtx_->SetTarget(surfaces_[frameIndex].bitmap.Get());
    d2dCtx_->BeginDraw();
    d2dCtx_->SetTransform(D2D1::Matrix3x2F::Identity());

    // Full-width bar across the top
    const float pad  = 8.0f;
    const float boxW = float(width_);
    const float boxH = 36.0f;
    D2D1_RECT_F bg = D2D1::RectF(0.0f, 0.0f, boxW, boxH);
    d2dCtx_->FillRectangle(bg, bgBrush_.Get());

    // Text
    D2D1_RECT_F textRect = D2D1::RectF(
        bg.left  + pad, bg.top    + pad,
        bg.right - pad, bg.bottom - pad);
    d2dCtx_->DrawText(
        wide.c_str(), static_cast<UINT32>(wide.size()),
        textFormat_.Get(), textRect, textBrush_.Get());

    d2dCtx_->EndDraw();
    d2dCtx_->SetTarget(nullptr);

    // Return wrapped resource back to D3D12.
    ctx_.on12->ReleaseWrappedResources(res11s, 1);
    ctx_.ctx11->Flush();
}
