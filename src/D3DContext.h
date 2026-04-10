// D3DContext.h
// Owns the shared D3D12 device, D3D11On12 device, and DXGI factory that every
// other subsystem borrows.
#pragma once
#include "Common.h"

struct D3DContext
{
    // ── D3D12 ───────────────────────────────────────────────────────────────
    ComPtr<ID3D12Device>       device12;
    ComPtr<ID3D12CommandQueue> cmdQueue12;   // DIRECT – shared with D3D11On12 + presenter
    ComPtr<ID3D12CommandQueue> dmlQueue12;   // DIRECT – dedicated to ORT/DML (avoids interference)

    // ── D3D11On12 ───────────────────────────────────────────────────────────
    ComPtr<ID3D11Device>        device11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11On12Device2>   on12;

    // ── DirectML ────────────────────────────────────────────────────────────
    ComPtr<IDMLDevice>          dmlDevice;

    // ── DXGI ────────────────────────────────────────────────────────────────
    ComPtr<IDXGIFactory6>      dxgiFactory;
    ComPtr<IDXGIAdapter1>      adapter;

    // Creates everything.  Throws std::runtime_error on failure.
    static D3DContext Create(bool enableDebug = false);

    // ── Helpers ─────────────────────────────────────────────────────────────

    // CPU-wait until cmdQueue12 finishes all pending work.
    void FlushQueue() const;
    void FlushDmlQueue() const;

    // Allocate a committed GPU buffer with optional UAV flags.
    ComPtr<ID3D12Resource> CreateBuffer(
        UINT64                sizeBytes,
        D3D12_RESOURCE_FLAGS  flags,
        D3D12_HEAP_TYPE       heapType  = D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON) const;

    // Allocate a 2-D texture.
    ComPtr<ID3D12Resource> CreateTexture2D(
        UINT                  width,
        UINT                  height,
        DXGI_FORMAT           format,
        D3D12_RESOURCE_FLAGS  flags,
        D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON) const;

    // Lightweight fence + event pair for single-use GPU/CPU sync.
    struct FenceSync
    {
        ComPtr<ID3D12Fence> fence;
        HANDLE              event = nullptr;
        UINT64              value = 0;

        // Signal the fence from queue and increment the stored value.
        void Signal(ID3D12CommandQueue* queue);
        // CPU-block until the fence reaches the last signalled value.
        void Wait();

        ~FenceSync() { if (event) CloseHandle(event); }

        // Move-only: raw HANDLE must not be double-closed.
        FenceSync() = default;
        FenceSync(const FenceSync&) = delete;
        FenceSync& operator=(const FenceSync&) = delete;

        FenceSync(FenceSync&& o) noexcept
            : fence(std::move(o.fence)), event(o.event), value(o.value)
        { o.event = nullptr; }

        FenceSync& operator=(FenceSync&& o) noexcept
        {
            if (this != &o) {
                if (event) CloseHandle(event);
                fence = std::move(o.fence);
                event = o.event;  o.event = nullptr;
                value = o.value;
            }
            return *this;
        }
    };

    FenceSync CreateFenceSync() const;

    // Drain and print any D3D12 debug messages to stdout.
    void DumpDebugMessages() const;

private:
    mutable ComPtr<ID3D12InfoQueue> infoQueue_;
};
