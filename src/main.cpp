// main.cpp – entry point for the Frame Interpolation MVP.
//
// Controls:
//   Q / Esc   – quit
//   I         – toggle RIFE interpolation on/off
//   M         – toggle metrics overlay on/off
//   D         – print device list to stdout
//   1-9       – switch to capture device N-1 (requires restart)
//
// Usage (live capture card):
//   framegen_mvp.exe [rife.onnx] [deviceIndex]
//
// Usage (file-based test, no capture card needed):
//   framegen_mvp.exe --file video.mp4 [rife.onnx]
//
// Usage (side-by-side compare: interpolated left, original right):
//   framegen_mvp.exe --file video.mp4 --compare [rife.onnx]
//
// Usage (disable overlay for driver/workaround testing):
//   framegen_mvp.exe --no-overlay [other flags]
//
// Usage (explicitly enable overlay):
//   framegen_mvp.exe --overlay [other flags]
//
// Usage (disable separate audio endpoint capture/playback):
//   framegen_mvp.exe --no-audio [other flags]
//
// Usage (GPU near-duplicate detection for 30-in-60 streams):
//   framegen_mvp.exe --gpu-dedupe [--dedupe-threshold N]
//
// Usage (30fps game in 60fps container, e.g. Xbox One S):
//   framegen_mvp.exe --half-rate [rife.onnx] [deviceIndex]
//
// Usage (30fps game → 120fps, requires 120Hz display):
//   framegen_mvp.exe --half-rate --4x [rife.onnx] [deviceIndex]
//
// Defaults: rife.onnx in the exe directory, device 0.
#include "Common.h"
#include "Pipeline.h"
#include "CaptureSource.h"
#include <shellapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------------------------------------------------------------------------
static Pipeline* g_pipeline = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (!g_pipeline) break;
        switch (wp)
        {
        case VK_ESCAPE:
        case 'Q':
            PostQuitMessage(0);
            break;
        case 'I':
            g_pipeline->ToggleInterpolation();
            printf("[main] interpolation: %s\n",
                   g_pipeline->IsInterpolating() ? "ON" : "OFF");
            fflush(stdout);
            break;
        case 'M':
            g_pipeline->ToggleOverlay();
            printf("[main] overlay: %s\n",
                   g_pipeline->IsOverlayVisible() ? "ON" : "OFF");
            fflush(stdout);
            break;
        case 'D':
        {
            auto devs = CaptureSource::EnumerateDevices();
            printf("[main] %zu capture device(s):\n", devs.size());
            for (size_t i = 0; i < devs.size(); ++i)
                wprintf(L"  [%zu] %ls\n", i, devs[i].friendlyName.c_str());
            fflush(stdout);
            break;
        }
        default:
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
static HWND CreateFullscreenWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FrameGenMVP";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Frame Generation MVP",
        WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);

    return hwnd;
}

// ---------------------------------------------------------------------------
int main(int, char**)
{
    SetConsoleOutputCP(CP_UTF8);  // Prevent garbled UTF-8 box chars in Windows console
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // ── Parse command line ──────────────────────────────────────────────────
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring onnxPath    = L"rife.onnx";
    UINT         deviceIndex = 0;
    std::wstring filePath;   // set via --file <path>; empty = use capture card
    bool         compareMode   = false; // --compare: side-by-side comparison window
    bool         noOverlay     = true;  // default safe mode: overlay disabled unless --overlay is passed
    bool         noAudio       = false; // --no-audio: disable separate audio endpoint path
    bool         gpuDedupe     = false; // --gpu-dedupe: compare frame content on GPU before RIFE
    UINT         dedupeThreshold = 12;  // --dedupe-threshold: sampled changed-pixel threshold
    bool         halfRateInput = false; // --half-rate: drop every other input frame
    bool         fourXMode    = false; // --4x: 3-pass 30→120fps (requires 120Hz display)
    bool         debugD3D     = false; // --debug: enable D3D12 debug layer
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg(argv[i]);
        if ((arg == L"--file" || arg == L"-f") && i + 1 < argc)
        {
            filePath = argv[++i];
        }
        else if (arg == L"--compare" || arg == L"-c")
        {
            compareMode = true;
        }
        else if (arg == L"--no-overlay")
        {
            noOverlay = true;
        }
        else if (arg == L"--overlay")
        {
            noOverlay = false;
        }
        else if (arg == L"--no-audio")
        {
            noAudio = true;
        }
        else if (arg == L"--gpu-dedupe")
        {
            gpuDedupe = true;
        }
        else if (arg == L"--dedupe-threshold" && i + 1 < argc)
        {
            dedupeThreshold = static_cast<UINT>(_wtoi(argv[++i]));
        }
        else if (arg == L"--half-rate" || arg == L"-h")
        {
            halfRateInput = true;
        }
        else if (arg == L"--4x")
        {
            fourXMode = true;
        }
        else if (arg == L"--debug")
        {
            debugD3D = true;
        }
        else if (arg.size() > 5 &&
                 (arg.substr(arg.size()-4) == L".mp4"  ||
                  arg.substr(arg.size()-4) == L".mkv"  ||
                  arg.substr(arg.size()-4) == L".mov"  ||
                  arg.substr(arg.size()-4) == L".avi"  ||
                  arg.substr(arg.size()-5) == L".mpeg"))
        {
            filePath = arg;   // bare video path without --file flag
        }
        else if (arg.size() > 5 && arg.substr(arg.size()-5) == L".onnx")
        {
            onnxPath = arg;
        }
        else
        {
            deviceIndex = static_cast<UINT>(_wtoi(arg.c_str()));
        }
    }

    LocalFree(argv);

    // ── Print available devices (skip if file mode) ─────────────────────────
    HR_CHECK(MFStartup(MF_VERSION));

    auto devices = CaptureSource::EnumerateDevices();
    if (filePath.empty())
    {
        // Live capture mode — require a capture card.
        if (devices.empty())
        {
            MessageBoxW(nullptr,
                L"No video capture devices found.\n"
                L"Connect a capture card and try again, or use\n"
                L"--file <video.mp4> to test without hardware.",
                L"Frame Generation MVP", MB_OK | MB_ICONWARNING);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        printf("[main] %zu capture device(s) found:\n", devices.size());
        for (size_t i = 0; i < devices.size(); ++i)
            wprintf(L"  [%zu] %ls\n", i, devices[i].friendlyName.c_str());

        if (deviceIndex >= (UINT)devices.size())
        {
            printf("[main] deviceIndex %u out of range, using 0\n", deviceIndex);
            deviceIndex = 0;
        }
        wprintf(L"[main] using device %u: %ls\n",
                deviceIndex, devices[deviceIndex].friendlyName.c_str());
    }
    else
    {
        // File mode — no capture card needed.
        wprintf(L"[main] file mode: %ls\n", filePath.c_str());
        if (!devices.empty())
        {
            printf("[main] (%zu capture device(s) also present but not used)\n",
                   devices.size());
        }
    }
    fflush(stdout);

    MFShutdown();  // Pipeline ctor calls MFStartup again

    // ── Create window ───────────────────────────────────────────────────────
    HWND hwnd = CreateFullscreenWindow(hInst);
    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Failed to create window.", L"Error", MB_OK);
        CoUninitialize();
        return 1;
    }

    // ── Build telemetry log path ────────────────────────────────────────────
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring logPath(exePath);
    auto lastSlash = logPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) logPath.resize(lastSlash + 1);
    logPath += L"framegen_log.csv";

    // ── Construct and start pipeline ────────────────────────────────────────
    Pipeline::Config cfg;
    cfg.deviceIndex  = deviceIndex;
    if (filePath.empty() && deviceIndex < (UINT)devices.size())
        cfg.captureDeviceName = devices[deviceIndex].friendlyName;
    cfg.filePath     = filePath;
    cfg.onnxPath     = onnxPath;
    cfg.logPath      = logPath;
    cfg.hwnd         = hwnd;
    cfg.debugD3D     = debugD3D;
    cfg.compareMode  = compareMode;
    cfg.noOverlay    = noOverlay;
    cfg.noAudio      = noAudio;
    cfg.gpuDedupe    = gpuDedupe;
    cfg.dedupeThreshold = dedupeThreshold;
    cfg.halfRateInput = halfRateInput;
    cfg.fourXMode    = fourXMode;
    cfg.screenW      = (UINT)GetSystemMetrics(SM_CXSCREEN);
    cfg.screenH      = (UINT)GetSystemMetrics(SM_CYSCREEN);

    printf("[main] overlay mode: %s\n", cfg.noOverlay ? "OFF (--no-overlay default)" : "ON (--overlay)");
    printf("[main] audio mode: %s\n", cfg.noAudio ? "OFF (--no-audio)" : "ON (default)");
    printf("[main] dedupe mode: %s (threshold=%u)\n", cfg.gpuDedupe ? "ON (--gpu-dedupe)" : "OFF", cfg.dedupeThreshold);
    fflush(stdout);

    int exitCode = 0;
    try
    {
        Pipeline pipeline(cfg);
        g_pipeline = &pipeline;

        printf("[main] starting pipeline (I = toggle RIFE, Q = quit)\n");
        fflush(stdout);

        pipeline.Start();

        // ── Message loop ────────────────────────────────────────────────────
        MSG msg = {};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            if (pipeline.ThreadFailed())
            {
                MessageBoxA(nullptr, pipeline.ThreadError().c_str(),
                            "Thread Error", MB_OK | MB_ICONERROR);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_pipeline = nullptr;
        pipeline.Stop();

        // Print final stats
        printf("\n--- Final telemetry ---\n%s\n",
               pipeline.GetTelemetry().StatsLine().c_str());
        wprintf(L"Log written to: %ls\n", logPath.c_str());
        fflush(stdout);
    }
    catch (const std::exception& e)
    {
        printf("[main] FATAL: %s\n", e.what()); fflush(stdout);
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    DestroyWindow(hwnd);
    CoUninitialize();
    return exitCode;
}
