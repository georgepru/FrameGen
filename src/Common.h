// Common.h – project-wide includes, helpers, and type aliases.
// Every translation unit includes this header first.
#pragma once

// ---- Windows / COM --------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>   // ComPtr<>
#include <dbghelp.h>

// ---- Direct3D 12 ----------------------------------------------------------
#include <d3d12.h>
#include <d3d12sdklayers.h>

// ---- Direct3D 11 / D3D11On12 ---------------------------------------------
#include <d3d11.h>
#include <d3d11on12.h>

// ---- DXGI -----------------------------------------------------------------
#include <dxgi1_6.h>

// ---- Media Foundation -----------------------------------------------------
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <evr.h>            // IMFDXGIDeviceManager

// ---- ONNX Runtime ---------------------------------------------------------
#include "onnxruntime_cxx_api.h"
#include "DirectML.h"
#include "dml_provider_factory.h"

// ---- STL ------------------------------------------------------------------
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- HRESULT helper -------------------------------------------------------
#ifndef HR_CHECK
#  define HR_CHECK(expr)                                                      \
     do {                                                                      \
       HRESULT _hr = (expr);                                                  \
       if (FAILED(_hr))                                                       \
       {                                                                      \
         char _buf[256];                                                      \
         snprintf(_buf, sizeof(_buf),                                         \
                  "%s(%d): HR_CHECK failed 0x%08X on: " #expr,               \
                  __FILE__, __LINE__, (unsigned)_hr);                         \
         throw std::runtime_error(_buf);                                      \
       }                                                                      \
     } while(0)
#endif

// ---- ORT helper -----------------------------------------------------------
#ifndef ORT_CHECK
#  define ORT_CHECK(expr)                                                     \
     do {                                                                     \
       OrtStatus* _st = (expr);                                               \
       if (_st) {                                                             \
         std::string _msg = Ort::GetApi().GetErrorMessage(_st);               \
         Ort::GetApi().ReleaseStatus(_st);                                    \
         throw std::runtime_error("ORT: " + _msg);                           \
       }                                                                      \
     } while(0)
#endif

// ---- Utility: round up to next multiple of 32 ----------------------------
inline UINT PadTo32(UINT v) { return (v + 31u) & ~31u; }

// ---- Crash log ------------------------------------------------------------
// Writes a timestamped crash entry to framegen_crash.log next to the exe.
// Thread-safe (internal mutex). Call from any catch block.
inline void WriteCrashLog(const char* source, const char* message)
{
    static std::mutex s_mu;
    std::lock_guard<std::mutex> lk(s_mu);

    // Resolve path next to the running exe.
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring logPath(exePath);
    auto slash = logPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) logPath.resize(slash + 1);
    logPath += L"framegen_crash.log";

    std::ofstream f(logPath, std::ios::app);
    if (!f) return;

    // Timestamp (local time).
    std::time_t now = std::time(nullptr);
    char timebuf[32] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    f << "[" << timebuf << "] " << source << ": " << message << "\n";
    f.flush();
}
