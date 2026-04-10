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
