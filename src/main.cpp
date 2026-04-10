// main.cpp – entry point for the Frame Interpolation MVP.
//
// Controls:
//   Q / Esc   – quit
//   I         – toggle RIFE interpolation on/off
//   D         – print device list to stdout
//   1-9       – switch to capture device N-1 (requires restart)
//
// Usage (live capture card):
//   framegen_mvp.exe [rife.onnx] [deviceIndex]
//
// Usage (file-based test, no capture card needed):
//   framegen_mvp.exe --file video.mp4 [rife.onnx]
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
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // ── Parse command line ──────────────────────────────────────────────────
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring onnxPath    = L"rife.onnx";
    UINT         deviceIndex = 0;
    std::wstring filePath;   // set via --file <path>; empty = use capture card

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg(argv[i]);
        if ((arg == L"--file" || arg == L"-f") && i + 1 < argc)
        {
            filePath = argv[++i];
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
    cfg.deviceIndex = deviceIndex;
    cfg.filePath    = filePath;
    cfg.onnxPath    = onnxPath;
    cfg.logPath     = logPath;
    cfg.hwnd        = hwnd;
    cfg.debugD3D    = false;

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
        printf("\n── Final telemetry ──\n%s\n",
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
