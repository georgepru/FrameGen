// RifeInference.cpp
#include "RifeInference.h"
#include <stdexcept>
#include <string>
#include <cstdio>
#include <chrono>

// ---------------------------------------------------------------------------
RifeInference::RifeInference(const std::wstring& onnxPath,
                             const D3DContext&   ctx,
                             UINT paddedWidth, UINT paddedHeight)
    : ctx_(ctx), pw_(paddedWidth), ph_(paddedHeight)
{
    // 1. Obtain DML provider API (zero-copy GPU allocation) ------------------
    Ort::GetApi().GetExecutionProviderApi(
        "DML", 0, reinterpret_cast<const void**>(&dmlApi_));

    // 2. Session options -----------------------------------------------------
    Ort::SessionOptions opts;
    opts.SetExecutionMode(ORT_SEQUENTIAL);
    opts.DisableMemPattern();  // required by DML EP
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // Use the dedicated DML queue so ORT/DML is isolated from D3D11On12.
    ORT_CHECK(OrtSessionOptionsAppendExecutionProviderEx_DML(
        opts, ctx_.dmlDevice.Get(), ctx_.dmlQueue12.Get()));

    if (!dmlApi_)
        throw std::runtime_error(
            "OrtDmlApi is null — DML EP not compiled in or API version mismatch.");

    // 3. Load ONNX model -----------------------------------------------------
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        session_ = Ort::Session(ortEnv_, onnxPath.c_str(), opts);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - t0).count();
        printf("[rife] session created in %lld ms\n", (long long)ms);
        fflush(stdout);
    }

    // 4. Allocate DML tensors and bind I/O -----------------------------------
    AllocateDmlTensors();

    binding_ = Ort::IoBinding(session_);
    binding_.BindInput("frame0", inVal0_);
    binding_.BindInput("frame1", inVal1_);
    binding_.BindOutput("mid",   outVal_);

    printf("[rife] ready (padded %ux%u)\n", pw_, ph_);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
RifeInference::~RifeInference()
{
    if (dmlApi_)
    {
        if (dmlAlloc0_)   dmlApi_->FreeGPUAllocation(dmlAlloc0_);
        if (dmlAlloc1_)   dmlApi_->FreeGPUAllocation(dmlAlloc1_);
        if (dmlAllocOut_) dmlApi_->FreeGPUAllocation(dmlAllocOut_);
    }
}

// ---------------------------------------------------------------------------
void RifeInference::AllocateDmlTensors()
{
    const std::array<int64_t, 4> shape = {
        1, 3,
        static_cast<int64_t>(ph_),
        static_cast<int64_t>(pw_)
    };
    const size_t bufBytes = static_cast<size_t>(pw_) * ph_ * 3 * sizeof(float);

    auto makeBuffer = [&](ComPtr<ID3D12Resource>& res)
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

        HR_CHECK(ctx_.device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&res)));
    };

    makeBuffer(inRes0_);
    makeBuffer(inRes1_);
    makeBuffer(outRes_);

    // Wrap D3D12 buffers as DML allocations (zero-copy).
    ORT_CHECK(dmlApi_->CreateGPUAllocationFromD3DResource(inRes0_.Get(),  &dmlAlloc0_));
    ORT_CHECK(dmlApi_->CreateGPUAllocationFromD3DResource(inRes1_.Get(),  &dmlAlloc1_));
    ORT_CHECK(dmlApi_->CreateGPUAllocationFromD3DResource(outRes_.Get(),  &dmlAllocOut_));

    Ort::MemoryInfo dmlMem("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);

    OrtValue* v0 = nullptr, *v1 = nullptr, *vO = nullptr;
    ORT_CHECK(Ort::GetApi().CreateTensorWithDataAsOrtValue(
        dmlMem.GetConst(), dmlAlloc0_, bufBytes,
        shape.data(), shape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v0));
    ORT_CHECK(Ort::GetApi().CreateTensorWithDataAsOrtValue(
        dmlMem.GetConst(), dmlAlloc1_, bufBytes,
        shape.data(), shape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v1));
    ORT_CHECK(Ort::GetApi().CreateTensorWithDataAsOrtValue(
        dmlMem.GetConst(), dmlAllocOut_, bufBytes,
        shape.data(), shape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vO));

    inVal0_ = Ort::Value{ v0 };
    inVal1_ = Ort::Value{ v1 };
    outVal_ = Ort::Value{ vO };
}

// ---------------------------------------------------------------------------
void RifeInference::Run()
{
    // Caller has already GPU-synced writes to InBuf0/InBuf1 before calling.
    Ort::RunOptions runOpts;
    session_.Run(runOpts, binding_);
    // SynchronizeOutputs() issues a CPU wait until DML finishes writing OutBuf.
    binding_.SynchronizeOutputs();
}
