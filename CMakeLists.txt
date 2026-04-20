cmake_minimum_required(VERSION 3.22)
project(FrameGenMVP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
#
# 1. ONNX Runtime with DirectML EP
#    Download the "directml" zip from:
#    https://github.com/microsoft/onnxruntime/releases
#    (e.g. onnxruntime-win-x64-directml-1.24.4.zip)
#    Extract into: deps/onnxruntime/
#    Expected layout:
#      deps/onnxruntime/include/onnxruntime_cxx_api.h
#      deps/onnxruntime/include/dml_provider_factory.h
#      deps/onnxruntime/lib/onnxruntime.lib
#      deps/onnxruntime/lib/onnxruntime.dll
#
# 2. Windows SDK (D3D12, D3D11, D3D11On12, DXGI, Media Foundation)
#    Installed with Visual Studio — no extra steps needed.
#
# Build:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Run:
#   build\Release\framegen_mvp.exe [rife.onnx]
# ─────────────────────────────────────────────────────────────────────────────

set(ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime")

if(NOT EXISTS "${ORT_DIR}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "ONNX Runtime not found at ${ORT_DIR}.\n"
        "Download the DirectML zip from GitHub releases and extract it to deps/onnxruntime/")
endif()

# ── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/D3DContext.cpp
    src/NearDuplicateDetector.cpp
    src/AudioPlayer.cpp
    src/AudioCaptureSource.cpp
    src/CaptureSource.cpp
    src/FileSource.cpp
    src/Overlay.cpp
    src/TextureConverter.cpp
    src/RifeInference.cpp
    src/SwapPresenter.cpp
    src/Pipeline.cpp
    src/Telemetry.cpp
)

add_executable(framegen_mvp ${SOURCES})

# ── Shader compilation ────────────────────────────────────────────────────────
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
file(MAKE_DIRECTORY ${SHADER_OUT})

function(compile_shader INPUT ENTRY PROFILE VARNAME OUTPUT_H)
    set(OUT "${SHADER_OUT}/${OUTPUT_H}")
    add_custom_command(
        OUTPUT  ${OUT}
        COMMAND fxc /nologo /T ${PROFILE} /E ${ENTRY} /Fh ${OUT} /Vn ${VARNAME} "${INPUT}"
        DEPENDS "${INPUT}"
        COMMENT "Compiling shader ${INPUT} (${ENTRY})"
    )
    target_sources(framegen_mvp PRIVATE ${OUT})
    set_source_files_properties(${OUT} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
endfunction()

compile_shader(${SHADER_DIR}/BGRAtoNCHW.hlsl  main   cs_5_0  g_BGRAtoNCHW    BGRAtoNCHW.h)
compile_shader(${SHADER_DIR}/NearDuplicateCS.hlsl main cs_5_0 g_NearDuplicateCS NearDuplicateCS.h)
compile_shader(${SHADER_DIR}/NCHWPresent.hlsl       VSMain vs_5_0  g_NCHWPresentVS    NCHWPresentVS.h)
compile_shader(${SHADER_DIR}/NCHWPresent.hlsl       PSMain ps_5_0  g_NCHWPresentPS    NCHWPresentPS.h)
compile_shader(${SHADER_DIR}/NCHWPresentFSRPS.hlsl  PSMain ps_5_0  g_NCHWPresentFSRPS NCHWPresentFSRPS.h)
compile_shader(${SHADER_DIR}/BGRAPresent.hlsl  VSMain vs_5_0  g_BGRAPresentVS BGRAPresentVS.h)
compile_shader(${SHADER_DIR}/BGRAPresent.hlsl  PSMain ps_5_0  g_BGRAPresentPS BGRAPresentPS.h)

target_include_directories(framegen_mvp PRIVATE
    src
    ${ORT_DIR}/include
    ${SHADER_OUT}
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(framegen_mvp PRIVATE
    "${ORT_DIR}/lib/onnxruntime.lib"

    # DirectX
    d3d12.lib
    d3d11.lib
    d2d1.lib
    dwrite.lib
    d3dcompiler.lib
    dxgi.lib
    dxguid.lib

    # Media Foundation (capture)
    mfplat.lib
    mfreadwrite.lib
    mfuuid.lib
    evr.lib
    strmiids.lib

    # Win32
    ole32.lib
    oleaut32.lib
    shell32.lib
    user32.lib
    gdi32.lib
    winmm.lib
)

# ── Copy runtime DLLs to output dir ──────────────────────────────────────────
add_custom_command(TARGET framegen_mvp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "${ORT_DIR}/lib/onnxruntime_providers_shared.dll"
        $<TARGET_FILE_DIR:framegen_mvp>
    COMMENT "Copying ORT DLLs"
)

# ── Compiler options ──────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(framegen_mvp PRIVATE
        /W4 /WX-
        /MP          # parallel compilation
        /Zi          # debug info in all configs
        /O2          # optimisation (Release) — cmake sets this per-config
        /EHa         # async exception handling: catch(...) catches SEH (access violations etc.)
    )
    target_compile_definitions(framegen_mvp PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        UNICODE _UNICODE
    )
endif()
