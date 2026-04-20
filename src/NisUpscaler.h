// NisUpscaler.h
// Two-pass NVIDIA Image Scaling (NVScaler) upscaler for D3D12.
//
// Pass 1 – NchwToTex: NCHW R16F planar buffer  →  Texture2D RGBA16F (srcTex)
// Pass 2 – NIS:       srcTex (RGBA16F)          →  dstTex (RGBA16F, UAV)
// Pass 3 – Blit:      dstTex (SRV)              →  backbuffer (RT)
//
// All three passes are recorded into a caller-supplied open command list via
// Dispatch().  The backbuffer enters and exits in PRESENT state.
#pragma once
#include "Common.h"
#include "D3DContext.h"

class NisUpscaler
{
public:
    // srcW/srcH: NCHW padded dimensions (texture will be this size).
    // dstW/dstH: backbuffer / output dimensions.
    // sharpness: 0.0 (soft) … 1.0 (sharp), default 0.5.
    NisUpscaler(const D3DContext& ctx,
                UINT srcW, UINT srcH,
                UINT dstW, UINT dstH,
                float sharpness = 0.5f);

    // Record all NIS work into an already-open command list.
    //   nchwBuf   – NCHW R16F planar resource from RIFE (state: COMMON)
    //   vidW/vidH – valid video pixel area inside the padded buffer
    //   stride    – padded width in pixels (== srcW passed to ctor)
    //   backBuf   – current swapchain back-buffer  (state: PRESENT → PRESENT)
    //   rtvHandle – CPU RTV handle for backBuf
    void Dispatch(ID3D12GraphicsCommandList* cmdList,
                  ID3D12Resource* nchwBuf,
                  UINT vidW, UINT vidH, UINT stride,
                  ID3D12Resource* backBuf,
                  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

private:
    void BuildNchwToTexPipeline();
    void BuildNisPipeline();
    void BuildBlitPipeline();
    void UploadCoefficients();

    const D3DContext& ctx_;
    UINT srcW_, srcH_, dstW_, dstH_;

    // ── Intermediate textures ─────────────────────────────────────────────
    ComPtr<ID3D12Resource> srcTex_;   // RGBA16F srcW x srcH  (UAV + SRV)
    ComPtr<ID3D12Resource> dstTex_;   // RGBA16F dstW x dstH  (UAV + SRV)

    // ── Coefficient textures (RGBA32F, 2x64, SRV, uploaded once) ─────────
    ComPtr<ID3D12Resource> coefScalerTex_;
    ComPtr<ID3D12Resource> coefUsmTex_;

    // ── NISConfig constant buffer (256-byte aligned, UPLOAD, persistently mapped) ──
    ComPtr<ID3D12Resource> nisCB_;

    // ── Shader-visible CBV_SRV_UAV heap ──────────────────────────────────
    //  Slot 0: NCHW buffer SRV  (updated each Dispatch call)
    //  Slot 1: srcTex UAV       (NchwToTex output)
    //  Slot 2: srcTex SRV       (NIS input t0)
    //  Slot 3: coefScaler SRV   (NIS t1)
    //  Slot 4: coefUsm SRV      (NIS t2)
    //  Slot 5: dstTex UAV       (NIS output)
    //  Slot 6: dstTex SRV       (blit input t0)
    static constexpr UINT HEAP_SLOTS = 7;
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descSize_ = 0;

    // ── NchwToTex pipeline ────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> nchwToTexRS_;
    ComPtr<ID3D12PipelineState> nchwToTexPSO_;

    // ── NIS pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> nisRS_;
    ComPtr<ID3D12PipelineState> nisPSO_;

    // ── Blit pipeline (VS reused from NCHWPresentVS, new blit PS) ────────
    ComPtr<ID3D12RootSignature> blitRS_;
    ComPtr<ID3D12PipelineState> blitPSO_;
};
