// Pipeline.cpp
#include "Pipeline.h"
#include "FileSource.h"
#include "Overlay.h"
#include <chrono>
#include <stdexcept>
#include <cstdio>

using namespace std::chrono;

// ---------------------------------------------------------------------------
Pipeline::Pipeline(const Config& cfg)
    : cfg_(cfg), hwnd_(cfg.hwnd)
{
    showOverlay_.store(!cfg_.noOverlay);

    HR_CHECK(MFStartup(MF_VERSION));

    // D3D context
    ctx_ = std::make_unique<D3DContext>(D3DContext::Create(cfg_.debugD3D));

    if (cfg_.gpuDedupe)
    {
        try
        {
            dedupeDetector_ = std::make_unique<NearDuplicateDetector>(*ctx_);
            printf("[pipeline] gpu dedupe enabled (threshold=%u)\n", cfg_.dedupeThreshold);
            fflush(stdout);
        }
        catch (const std::exception& ex)
        {
            printf("[pipeline] gpu dedupe init failed (%s) -- disabling\n", ex.what());
            fflush(stdout);
            dedupeDetector_.reset();
        }
    }

    // Telemetry
    telemetry_ = std::make_unique<Telemetry>(cfg_.logPath);

    // Capture source — queries native resolution during construction.
    UINT vidW = 0, vidH = 0;
    if (!cfg_.filePath.empty())
    {
        fileSource_ = std::make_unique<FileSource>(
            cfg_.filePath, captureQueue_, *ctx_);
        vidW = fileSource_->NativeWidth();
        vidH = fileSource_->NativeHeight();
    }
    else
    {
        capture_ = std::make_unique<CaptureSource>(
            cfg_.deviceIndex, captureQueue_, *ctx_);
        if (!cfg_.noAudio)
        {
            try
            {
                audioCapture_ = std::make_unique<AudioCaptureSource>(cfg_.captureDeviceName);
            }
            catch (const std::exception& ex)
            {
                printf("[audio] disabled: %s\n", ex.what());
                fflush(stdout);
                audioCapture_.reset();
            }
        }
        else
        {
            printf("[audio] disabled (--no-audio)\n");
            fflush(stdout);
        }
        vidW = capture_->NativeWidth();
        vidH = capture_->NativeHeight();
    }

    if (vidW == 0 || vidH == 0)
        throw std::runtime_error("CaptureSource returned zero video dimensions");

    const UINT pw = PadTo32(vidW);
    const UINT ph = PadTo32(vidH);

    printf("[pipeline] video %ux%u -> padded %ux%u\n", vidW, vidH, pw, ph);
    fflush(stdout);

    converter_ = std::make_unique<TextureConverter>(*ctx_);
    rife_      = std::make_unique<RifeInference>(cfg_.onnxPath, *ctx_, pw, ph);
    presenter_ = std::make_unique<SwapPresenter>(hwnd_, *ctx_, pw, ph,
                     cfg_.compareMode, cfg_.screenW, cfg_.screenH);

    // Allocate reference BGRA texture for compare mode (right-half original frame)
    if (cfg_.compareMode && vidW > 0 && vidH > 0)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = vidW;
        rd.Height           = vidH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

        HR_CHECK(ctx_->device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&refTex_)));
        printf("[pipeline] compare mode: reference BGRA texture allocated\n");
        fflush(stdout);
    }

    // Overlay (D2D text on top of swap chain back buffers).
    // Skipped in compare mode: swapchain is full-screen dims, not padded-video dims.
    if (!cfg_.compareMode && !cfg_.noOverlay)
    {
        try
        {
            overlay_ = std::make_unique<Overlay>(
                *ctx_, presenter_->SwapChain(),
                SwapPresenter::BUFFER_COUNT, pw, ph);
            presenter_->SetOverlay(overlay_.get());
            printf("[pipeline] overlay ready\n"); fflush(stdout);
        }
        catch (const std::exception& ex)
        {
            printf("[pipeline] overlay init failed (%s) -- continuing without\n",
                   ex.what()); fflush(stdout);
            overlay_ = nullptr;
        }
    }
    else if (cfg_.noOverlay)
    {
        printf("[pipeline] overlay disabled (--no-overlay)\n"); fflush(stdout);
    }

    // Allocate scratch buffers for 4x recursive interpolation.
    if (cfg_.fourXMode)
    {
        const size_t bufBytes =
            static_cast<size_t>(pw) * ph * 3 * sizeof(uint16_t);

        auto makeScratch = [&](ComPtr<ID3D12Resource>& res)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = bufBytes;
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            HR_CHECK(ctx_->device12->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&res)));
        };

        makeScratch(scratch0_);
        makeScratch(scratch1_);
        makeScratch(scratch2_);
        makeScratch(scratch3_);
        printf("[pipeline] 4x mode: 4 scratch buffers allocated (%.1f MB each)\n",
               bufBytes / (1024.0 * 1024.0));
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
Pipeline::~Pipeline()
{
    Stop();
    MFShutdown();
}

// ---------------------------------------------------------------------------
void Pipeline::Start()
{
    running_ = true;
    if (fileSource_) fileSource_->Start();
    else             capture_->Start();
    if (audioCapture_) audioCapture_->Start();

    captureThread_ = std::thread([this]{ CaptureThread(); });
    rifeThread_    = std::thread([this]{ RifeThread();    });
    presentThread_ = std::thread([this]{ PresentThread(); });
}

// ---------------------------------------------------------------------------
void Pipeline::Stop()
{
    running_ = false;
    captureQueue_.Interrupt();
    presentQueue_.Interrupt();

    if (capture_)    capture_->Stop();
    if (audioCapture_) audioCapture_->Stop();
    if (fileSource_) fileSource_->Stop();

    if (captureThread_.joinable()) captureThread_.join();
    if (rifeThread_.joinable())    rifeThread_.join();
    if (presentThread_.joinable()) presentThread_.join();
}

// ---------------------------------------------------------------------------
// CaptureThread — just keeps MF alive; actual frames arrive via MF callback.
// ---------------------------------------------------------------------------
void Pipeline::CaptureThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running_)
        std::this_thread::sleep_for(milliseconds(5));

    CoUninitialize();
}

// ---------------------------------------------------------------------------
// RifeThread — pairs frames, converts to NCHW, runs inference, enqueues present.
// ---------------------------------------------------------------------------
void Pipeline::RifeThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("[rife thread] started\n"); fflush(stdout);

    try
    {
        const UINT pw = rife_->PaddedWidth();
        const UINT ph = rife_->PaddedHeight();

        auto fence = ctx_->CreateFenceSync();

        std::optional<CapturedFrame> prevFrame;
        bool skipNext = false; // used by halfRateInput to drop every other frame

        while (running_)
        {
            auto frameOpt = captureQueue_.Pop();
            if (!frameOpt) break;

            CapturedFrame frame = std::move(*frameOpt);
            telemetry_->OnCaptureFrame();

            // ── Gap detection: reset state after a capture hiccup ────────────
            // UnwrapUnderlyingResource failures (e.g. Xbox signal drop / format
            // change) cause CaptureSource to skip frames silently.  If the gap
            // between this frame and the last one we actually processed is long
            // (> 300 ms), the prevFrame is stale — interpolating across that gap
            // would produce garbage.  Reset so we re-seed cleanly.
            if (prevFrame)
            {
                LARGE_INTEGER qpcFreq;
                QueryPerformanceFrequency(&qpcFreq);
                double gapMs = static_cast<double>(
                    frame.captureTime - prevFrame->captureTime)
                    * 1000.0 / qpcFreq.QuadPart;
                if (gapMs > 300.0)
                {
                    printf("[rife] %.0f ms gap detected, resetting frame pair\n", gapMs);
                    fflush(stdout);
                    if (prevFrame->tex11)
                    {
                        ctx_->on12->ReturnUnderlyingResource(
                            prevFrame->tex11.Get(), 0, nullptr, nullptr);
                        prevFrame->tex11 = nullptr;
                    }
                    prevFrame.reset();
                    skipNext = false; // re-sync half-rate counter
                }
            }

            // ── GPU near-duplicate drop (content-based) ────────────────────
            // If enabled, compare this frame against the previous kept frame
            // on a reduced GPU sampling grid and skip near-identical frames.
            if (cfg_.gpuDedupe && dedupeDetector_ && prevFrame)
            {
                UINT changed = 0;
                if (dedupeDetector_->IsNearDuplicate(
                        prevFrame->texBGRA.Get(),
                        frame.texBGRA.Get(),
                        frame.width,
                        frame.height,
                        cfg_.dedupeThreshold,
                        &changed))
                {
                    if (frame.tex11)
                    {
                        ctx_->on12->ReturnUnderlyingResource(
                            frame.tex11.Get(), 0, nullptr, nullptr);
                        frame.tex11 = nullptr;
                    }
                    continue;
                }
            }

            // ── Half-rate input: drop every other frame ──────────────────────
            // Xbox One S (and similar consoles) lock 30fps games to a 60fps
            // container, resulting in duplicate frames.  Dropping every other
            // frame gives RIFE unique A/B pairs instead of wasted A/A pairs.
            if (cfg_.halfRateInput && !(cfg_.gpuDedupe && dedupeDetector_))
            {
                if (skipNext)
                {
                    if (frame.tex11)
                    {
                        ctx_->on12->ReturnUnderlyingResource(
                            frame.tex11.Get(), 0, nullptr, nullptr);
                        frame.tex11 = nullptr;
                    }
                    skipNext = false;
                    continue;
                }
                skipNext = true;
            }

            if (!interpolation_.load())
            {
                // Pass-through: push BGRA frame directly to present queue.
                PresentFrame pf;
                pf.nchwBuf = nullptr;  // signals pass-through to PresentThread
                pf.vidW    = frame.width;
                pf.vidH    = frame.height;
                pf.paddedW = pw;
                pf.paddedH = ph;

                // Return D3D11 resource before presenting.
                if (frame.tex11)
                {
                    ctx_->on12->ReturnUnderlyingResource(
                        frame.tex11.Get(), 0, nullptr, nullptr);
                    frame.tex11 = nullptr;
                }

                presentQueue_.Push(pf);
                prevFrame.reset();
                continue;
            }

            // ── Seed first frame ────────────────────────────────────────────
            if (!prevFrame)
            {
        printf("[rife] seeding frame 0\n"); fflush(stdout);
                converter_->BGRAtoNCHW(
                    frame.texBGRA.Get(), rife_->InBuf0(), nullptr,
                    frame.width, frame.height, pw, ph, fence);
                fence.Wait();

                if (frame.tex11)
                {
                    ctx_->on12->ReturnUnderlyingResource(
                        frame.tex11.Get(), 0, nullptr, nullptr);
                    frame.tex11 = nullptr;
                }

                prevFrame = std::move(frame);
                continue;
            }

            // ── Convert current frame to InBuf1 ─────────────────────────────
            // Also copies BGRA to refTex_ in compare mode (refTex_ is null otherwise).
            converter_->BGRAtoNCHW(
                frame.texBGRA.Get(), rife_->InBuf1(), refTex_.Get(),
                frame.width, frame.height, pw, ph, fence);
            fence.Wait();

            // Return previous frame's D3D11 resource.
            if (prevFrame->tex11)
            {
                ctx_->on12->ReturnUnderlyingResource(
                    prevFrame->tex11.Get(), 0, nullptr, nullptr);
                prevFrame->tex11 = nullptr;
            }

            if (cfg_.fourXMode)
            {
                // ── 4x recursive interpolation: A,B → Q1,M,Q3,B ────────────
                // Pass 1: Run(A, B) → M
                // Pass 2: Run(A, M) → Q1   (t=0.25)
                // Pass 3: Run(M, B) → Q3   (t=0.75)
                // Output order pushed to present: Q1, M, Q3, B
                //
                // scratch0_ = M = interp(A,B)
                // scratch1_ = B (saved before InBuf1 is overwritten)
                //
                // Safety: scratch0/1 are only used as CopyBuffer SOURCE alongside
                // PresentThread GPU reads — concurrent READs are safe.

                // Save B before InBuf1 gets clobbered.
                converter_->CopyBuffer(rife_->InBuf1(), scratch1_.Get(), pw, ph, fence);
                fence.Wait();  // scratch1 = B

                // Pass 1: Run(A, B) → M
                telemetry_->OnRifeStart();
                rife_->Run();
                telemetry_->OnRifeEnd();
                converter_->CopyBuffer(rife_->OutBuf(), scratch0_.Get(), pw, ph, fence);
                fence.Wait();  // scratch0 = M

                // Pass 2: Run(A, M) → Q1
                converter_->CopyBuffer(scratch0_.Get(), rife_->InBuf1(), pw, ph, fence);
                fence.Wait();  // InBuf1 = M
                telemetry_->OnRifeStart();
                rife_->Run();
                telemetry_->OnRifeEnd();

                // Copy Q1 from OutBuf into scratch2_ immediately.
                // OutBuf will be overwritten by Pass 3 on dmlQueue12, which runs
                // on a different queue from cmdQueue12 (PresentThread's reader).
                // scratch2_ = Q1 is read-only from here; won't be touched again
                // until the next 4x cycle, giving PresentThread ample time.
                converter_->CopyBuffer(rife_->OutBuf(), scratch2_.Get(), pw, ph, fence);
                fence.Wait();  // scratch2_ = Q1 (stable for PresentThread)

                // Push Q1 (scratch2_, not OutBuf — OutBuf is now free for Pass 3).
                {
                    PresentFrame pf;
                    pf.nchwBuf = scratch2_.Get();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW = frame.width; pf.vidH = frame.height;
                    pf.paddedW = pw;       pf.paddedH = ph;
                    if (!presentQueue_.Push(pf)) telemetry_->OnDroppedFrame();
                }

                // Push M (scratch0). CopyBuffer below also READs scratch0 — safe.
                {
                    PresentFrame pf;
                    pf.nchwBuf = scratch0_.Get();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW = frame.width; pf.vidH = frame.height;
                    pf.paddedW = pw;       pf.paddedH = ph;
                    if (!presentQueue_.Push(pf)) telemetry_->OnDroppedFrame();
                }

                // Pass 3: Run(M, B) → Q3
                converter_->CopyBuffer(scratch0_.Get(), rife_->InBuf0(), pw, ph, fence);
                fence.Wait();  // InBuf0 = M
                converter_->CopyBuffer(scratch1_.Get(), rife_->InBuf1(), pw, ph, fence);
                fence.Wait();  // InBuf1 = B
                telemetry_->OnRifeStart();
                rife_->Run();
                telemetry_->OnRifeEnd();

                // Copy Q3 to scratch3_ — OutBuf will be overwritten by the next
                // pair's Pass 1 Run() which can start as soon as 3ms from now if
                // a captured frame is already waiting in captureQueue_.
                converter_->CopyBuffer(rife_->OutBuf(), scratch3_.Get(), pw, ph, fence);
                fence.Wait();  // scratch3_ = Q3 (stable)

                // Push Q3 (scratch3_, not OutBuf).
                {
                    PresentFrame pf;
                    pf.nchwBuf = scratch3_.Get();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW = frame.width; pf.vidH = frame.height;
                    pf.paddedW = pw;       pf.paddedH = ph;
                    if (!presentQueue_.Push(pf)) telemetry_->OnDroppedFrame();
                }

                // Shift B → InBuf0 for next cycle. Also READs scratch1 — safe.
                converter_->CopyBuffer(scratch1_.Get(), rife_->InBuf0(), pw, ph, fence);
                fence.Wait();  // InBuf0 = B

                // Push B (InBuf0).
                {
                    PresentFrame pf;
                    pf.nchwBuf = rife_->InBuf0();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW = frame.width; pf.vidH = frame.height;
                    pf.paddedW = pw;       pf.paddedH = ph;
                    if (!presentQueue_.Push(pf)) telemetry_->OnDroppedFrame();
                }
            }
            else
            {
                // ── 2x inference (default) ───────────────────────────────────
                telemetry_->OnRifeStart();
                rife_->Run();
                telemetry_->OnRifeEnd();

                // Push interpolated mid frame.
                {
                    PresentFrame pf;
                    pf.nchwBuf = rife_->OutBuf();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW    = frame.width;
                    pf.vidH    = frame.height;
                    pf.paddedW = pw;
                    pf.paddedH = ph;
                    pf.needsDwmFlushAfter = true; // wait for DWM refresh before presenting original
                    if (!presentQueue_.Push(pf))
                        telemetry_->OnDroppedFrame();
                }

                // Shift: InBuf1 → InBuf0 for next pair.
                converter_->CopyBuffer(rife_->InBuf1(), rife_->InBuf0(), pw, ph, fence);
                fence.Wait();

                // Push original frame (InBuf0 stable until next CopyBuffer ~33ms away).
                {
                    PresentFrame pf;
                    pf.nchwBuf = rife_->InBuf0();
                    pf.bgraRef = refTex_.Get();
                    pf.vidW    = frame.width;
                    pf.vidH    = frame.height;
                    pf.paddedW = pw;
                    pf.paddedH = ph;
                    if (!presentQueue_.Push(pf))
                        telemetry_->OnDroppedFrame();
                }
            }

            // Return current frame's D3D11 resource.
            if (frame.tex11)
            {
                ctx_->on12->ReturnUnderlyingResource(
                    frame.tex11.Get(), 0, nullptr, nullptr);
                frame.tex11 = nullptr;
            }

            prevFrame = std::move(frame);
        }
    }
    catch (const std::exception& e)
    {
        threadError_  = std::string("RIFE thread: ") + e.what();
        threadFailed_ = true;
        printf("[rife] EXCEPTION: %s\n", e.what()); fflush(stdout);
        running_ = false;
        captureQueue_.Interrupt();
        presentQueue_.Interrupt();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
    catch (...)
    {
        threadError_  = "RIFE thread: unknown exception";
        threadFailed_ = true;
        printf("[rife] UNKNOWN EXCEPTION\n"); fflush(stdout);
        running_ = false;
        captureQueue_.Interrupt();
        presentQueue_.Interrupt();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    CoUninitialize();
}

// ---------------------------------------------------------------------------
// PresentThread — waits for a PresentFrame and hands it to SwapPresenter.
// ---------------------------------------------------------------------------
void Pipeline::PresentThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("[present thread] started\n"); fflush(stdout);

    try
    {
        while (running_)
        {
            auto pfOpt = presentQueue_.Pop();
            if (!pfOpt) break;

            const PresentFrame& pf = *pfOpt;

            if (pf.nchwBuf)
            {
                std::string stats = showOverlay_ ? telemetry_->StatsLine() : std::string{};
                presenter_->Present(pf.nchwBuf, pf.vidW, pf.vidH,
                                    pf.paddedW, pf.paddedH, stats.c_str(),
                                    pf.bgraRef);

                // For interpolated frames: spin-wait until ~one vsync period has
                // elapsed since this present before allowing the original frame
                // present. This keeps both frames in separate vsync intervals
                // without touching any GPU/DWM API (which TDRs on RTX 5080).
                if (pf.needsDwmFlushAfter)
                {
                    using Clock = std::chrono::steady_clock;
                    using Ms    = std::chrono::duration<double, std::milli>;
                    // Budget: 16.67ms at 60Hz. Sleep most of it, busy-spin the tail.
                    constexpr double kVsyncMs  = 1000.0 / 60.0;
                    constexpr double kSleepMs  = kVsyncMs - 2.0; // leave 2ms tail for spin
                    auto deadline = Clock::now() + Ms(kVsyncMs);
                    std::this_thread::sleep_for(Ms(kSleepMs));
                    while (Clock::now() < deadline) { /* spin */ }
                }
            }
            else
            {
                // Pass-through: nothing to present from RIFE; just update display.
                // In a future iteration we could blit the BGRA directly here.
            }

            telemetry_->SetWaitMs(presenter_->LastWaitMs());
            telemetry_->OnPresent();
        }
    }
    catch (const std::exception& e)
    {
        // If device was removed, log the detailed reason.
        HRESULT removedReason = ctx_->device12->GetDeviceRemovedReason();
        char removedBuf[128] = {};
        if (FAILED(removedReason))
            snprintf(removedBuf, sizeof(removedBuf),
                     " [DeviceRemovedReason=0x%08X]", (unsigned)removedReason);

        threadError_  = std::string("Present thread: ") + e.what() + removedBuf;
        threadFailed_ = true;
        printf("[present] EXCEPTION: %s%s\n", e.what(), removedBuf); fflush(stdout);
        running_ = false;
        captureQueue_.Interrupt();
        presentQueue_.Interrupt();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
    catch (...)
    {
        threadError_  = "Present thread: unknown exception";
        threadFailed_ = true;
        printf("[present] UNKNOWN EXCEPTION\n"); fflush(stdout);
        running_ = false;
        captureQueue_.Interrupt();
        presentQueue_.Interrupt();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    CoUninitialize();
}
