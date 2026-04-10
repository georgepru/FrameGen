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
    HR_CHECK(MFStartup(MF_VERSION));

    // D3D context
    ctx_ = std::make_unique<D3DContext>(D3DContext::Create(cfg_.debugD3D));

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
    presenter_ = std::make_unique<SwapPresenter>(hwnd_, *ctx_, pw, ph);

    // Overlay (D2D text on top of swap chain back buffers)
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

        while (running_)
        {
            auto frameOpt = captureQueue_.Pop();
            if (!frameOpt) break;

            CapturedFrame frame = std::move(*frameOpt);
            telemetry_->OnCaptureFrame();

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
                    frame.texBGRA.Get(), rife_->InBuf0(),
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
            converter_->BGRAtoNCHW(
                frame.texBGRA.Get(), rife_->InBuf1(),
                frame.width, frame.height, pw, ph, fence);
            fence.Wait();

            // Return previous frame's D3D11 resource.
            if (prevFrame->tex11)
            {
                ctx_->on12->ReturnUnderlyingResource(
                    prevFrame->tex11.Get(), 0, nullptr, nullptr);
                prevFrame->tex11 = nullptr;
            }

            // ── RIFE inference ──────────────────────────────────────────────
            printf("[rife] running inference\n"); fflush(stdout);
            telemetry_->OnRifeStart();
            rife_->Run();
            printf("[rife] inference done\n"); fflush(stdout);
            telemetry_->OnRifeEnd();

            // ── Push interpolated (mid) frame to presenter ───────────────────
            {
                PresentFrame pf;
                pf.nchwBuf = rife_->OutBuf();
                pf.vidW    = frame.width;
                pf.vidH    = frame.height;
                pf.paddedW = pw;
                pf.paddedH = ph;
                if (!presentQueue_.Push(pf))
                    telemetry_->OnDroppedFrame();
            }

            // Shift: InBuf1 → InBuf0 for next pair (copy on GPU).
            // After this, InBuf0 holds the current original frame.
            converter_->CopyBuffer(rife_->InBuf1(), rife_->InBuf0(), pw, ph, fence);
            fence.Wait();

            // ── Push original (current) frame to presenter ───────────────────
            // InBuf0 is stable until the next iteration's CopyBuffer overwrites it
            // (~65 ms away at 30 fps input).  PresentThread's WaitForSingleObject
            // at 60 Hz takes ≤ 17 ms, so its cmdQueue12 draw is always submitted
            // before the overwrite, keeping the queue safely ordered.
            {
                PresentFrame pf;
                pf.nchwBuf = rife_->InBuf0();
                pf.vidW    = frame.width;
                pf.vidH    = frame.height;
                pf.paddedW = pw;
                pf.paddedH = ph;
                if (!presentQueue_.Push(pf))
                    telemetry_->OnDroppedFrame();
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
                std::string stats = telemetry_->StatsLine();
                presenter_->Present(pf.nchwBuf, pf.vidW, pf.vidH,
                                    pf.paddedW, pf.paddedH, stats.c_str());
            }
            else
            {
                // Pass-through: nothing to present from RIFE; just update display.
                // In a future iteration we could blit the BGRA directly here.
            }

            telemetry_->OnPresent();
        }
    }
    catch (const std::exception& e)
    {
        threadError_  = std::string("Present thread: ") + e.what();
        threadFailed_ = true;
        printf("[present] EXCEPTION: %s\n", e.what()); fflush(stdout);
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
