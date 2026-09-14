#include "native_renderer_d3d12.h"

#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#endif

namespace rerevved::gpu
{

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

namespace
{

constexpr std::uint32_t kFrameCount         = 2;
constexpr DWORD         kFenceWaitTimeoutMs = 5000;

bool logFailure(const char* operation, HRESULT result)
{
    REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
    return false;
}

void enableDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    const HRESULT                                    result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(result))
    {
        REXLOG_WARN("Native D3D12 DRED settings unavailable: HRESULT 0x{:08X}",
                    static_cast<std::uint32_t>(result));
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    REXLOG_INFO("Native D3D12 DRED enabled");
}

bool enableReplayDebugLayer()
{
    ComPtr<ID3D12Debug> debug;
    const HRESULT       result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(result))
    {
        REXLOG_WARN("Native D3D12 replay debug layer unavailable: HRESULT 0x{:08X}",
                    static_cast<std::uint32_t>(result));
        return false;
    }
    debug->EnableDebugLayer();
    REXLOG_INFO("Native D3D12 replay debug layer enabled");
    return true;
}

bool compileReplayShader(const char*       source,
                         const char*       entryPoint,
                         const char*       profile,
                         ComPtr<ID3DBlob>& bytecode)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT    result = D3DCompile(source,
                                         std::strlen(source),
                                         "native_draw_replay_helper",
                                         nullptr,
                                         nullptr,
                                         entryPoint,
                                         profile,
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                         0,
                                         &bytecode,
                                         &errors);
    if (FAILED(result))
    {
        if (errors)
        {
            REXLOG_ERROR("Native D3D12 replay helper shader failed: {}",
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return logFailure("replay helper shader compilation", result);
    }
    return true;
}

D3D12_BLEND mapBlendFactor(NativeDrawReplayBlendFactor factor)
{
    switch (factor)
    {
        case NativeDrawReplayBlendFactor::Zero:
            return D3D12_BLEND_ZERO;
        case NativeDrawReplayBlendFactor::One:
            return D3D12_BLEND_ONE;
        case NativeDrawReplayBlendFactor::SourceColor:
            return D3D12_BLEND_SRC_COLOR;
        case NativeDrawReplayBlendFactor::InverseSourceColor:
            return D3D12_BLEND_INV_SRC_COLOR;
        case NativeDrawReplayBlendFactor::SourceAlpha:
            return D3D12_BLEND_SRC_ALPHA;
        case NativeDrawReplayBlendFactor::InverseSourceAlpha:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case NativeDrawReplayBlendFactor::DestinationAlpha:
            return D3D12_BLEND_DEST_ALPHA;
        case NativeDrawReplayBlendFactor::InverseDestinationAlpha:
            return D3D12_BLEND_INV_DEST_ALPHA;
        case NativeDrawReplayBlendFactor::DestinationColor:
            return D3D12_BLEND_DEST_COLOR;
        case NativeDrawReplayBlendFactor::InverseDestinationColor:
            return D3D12_BLEND_INV_DEST_COLOR;
        case NativeDrawReplayBlendFactor::SourceAlphaSaturated:
            return D3D12_BLEND_SRC_ALPHA_SAT;
        case NativeDrawReplayBlendFactor::BlendFactor:
            return D3D12_BLEND_BLEND_FACTOR;
        case NativeDrawReplayBlendFactor::InverseBlendFactor:
            return D3D12_BLEND_INV_BLEND_FACTOR;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP mapBlendOp(NativeDrawReplayBlendOp op)
{
    switch (op)
    {
        case NativeDrawReplayBlendOp::Add:
            return D3D12_BLEND_OP_ADD;
        case NativeDrawReplayBlendOp::Subtract:
            return D3D12_BLEND_OP_SUBTRACT;
        case NativeDrawReplayBlendOp::ReverseSubtract:
            return D3D12_BLEND_OP_REV_SUBTRACT;
        case NativeDrawReplayBlendOp::Minimum:
            return D3D12_BLEND_OP_MIN;
        case NativeDrawReplayBlendOp::Maximum:
            return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

DXGI_FORMAT mapTextureFormat(NativeDrawReplayTextureFormat format)
{
    switch (format)
    {
        case NativeDrawReplayTextureFormat::Rgba8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case NativeDrawReplayTextureFormat::R8:
            return DXGI_FORMAT_R8_UNORM;
        case NativeDrawReplayTextureFormat::Bc1:
            return DXGI_FORMAT_BC1_UNORM;
        case NativeDrawReplayTextureFormat::Bc2:
            return DXGI_FORMAT_BC2_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

constexpr const char* kReplayCopyShaderSource = R"hlsl(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput copy_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = positions[vertex_id] * 0.5 + 0.5;
    return output;
}

Texture2D<float4> source_texture : register(t0);
cbuffer CopyConstants : register(b0) {
    uint sample_a;
    uint sample_b;
    uint sample_count;
    uint padding;
};

float4 copy_ps(VSOutput input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    return source_texture.Load(int3(pixel, 0));
}
)hlsl";

constexpr const char* kReplayResolveShaderSource = R"hlsl(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput copy_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = positions[vertex_id] * 0.5 + 0.5;
    return output;
}

Texture2DMS<float4> source_texture : register(t0);
cbuffer CopyConstants : register(b0) {
    uint sample_a;
    uint sample_b;
    uint sample_count;
    uint padding;
};

float4 copy_ps(VSOutput input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    if (sample_a == sample_b) {
        return source_texture.Load(pixel, sample_a);
    }
    return 0.5 * (source_texture.Load(pixel, sample_a) + source_texture.Load(pixel, sample_b));
}
)hlsl";

bool createReplayUploadBuffer(ID3D12Device*           device,
                              const void*             data,
                              std::size_t             size,
                              ComPtr<ID3D12Resource>& resource)
{
    if (size == 0)
    {
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result        = device->CreateCommittedResource(&heap,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(&resource));
    if (FAILED(result))
    {
        return false;
    }
    void* mapped = nullptr;
    result       = resource->Map(0, nullptr, &mapped);
    if (FAILED(result))
    {
        resource.Reset();
        return false;
    }
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
    return true;
}

std::uint32_t alignConstantBytes(std::size_t size)
{
    return static_cast<std::uint32_t>((size + 255U) & ~std::size_t(255U));
}

} // namespace
#endif

struct NativeRendererD3D12::Impl
{
    std::thread             rendererThread;
    std::mutex              stateMutex;
    std::condition_variable stateCv;
    bool                    stopRequested           = false;
    bool                    initializationDone      = false;
    bool                    initializationSucceeded = false;
    bool                    resizePending           = false;
    std::uint32_t           requestedWidth          = 0;
    std::uint32_t           requestedHeight         = 0;
    bool                    runtimeFailed           = false;
    std::function<void()>   requestDeferredQuit;
    std::atomic<bool>       initialized{ false };
    std::atomic<bool>       gpuObjectsAbandoned{ false };

#if defined(_WIN32)
    ComPtr<IDXGIFactory6>                                   factory;
    ComPtr<IDXGIAdapter1>                                   adapter;
    ComPtr<ID3D12Device>                                    device;
    ComPtr<ID3D12CommandQueue>                              queue;
    ComPtr<IDXGISwapChain3>                                 swapChain;
    ComPtr<ID3D12DescriptorHeap>                            rtvHeap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount>         backBuffers;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators;
    ComPtr<ID3D12GraphicsCommandList>                       commandList;
    ComPtr<ID3D12Fence>                                     fence;
    ComPtr<ID3D12InfoQueue>                                 replayInfoQueue;
    std::array<std::uint64_t, kFrameCount>                  fenceValues{};
    std::uint64_t                                           nextFenceValue = 1;
    HANDLE                                                  fenceEvent     = nullptr;
    std::uint32_t                                           rtvStride      = 0;
    std::uint32_t                                           width          = 0;
    std::uint32_t                                           height         = 0;

    // Every object in this bundle remains owned by Impl until the replay
    // fence has completed. On a live-device drain failure shutdown detaches
    // the bundle together with the regular renderer objects.
    ComPtr<ID3D12RootSignature>           replayRootSignature;
    ComPtr<ID3D12PipelineState>           replayPipelineState;
    ComPtr<ID3D12RootSignature>           replayCopyRootSignature;
    ComPtr<ID3D12PipelineState>           replayCopyPipelineState;
    ComPtr<ID3D12PipelineState>           replayCopyPipelineStateSample3;
    ComPtr<ID3D12PipelineState>           replayResolvePipelineState;
    ComPtr<ID3D12Resource>                replayVertexBuffer;
    ComPtr<ID3D12Resource>                replayIndexBuffer;
    ComPtr<ID3D12Resource>                replayVertexConstants;
    ComPtr<ID3D12Resource>                replayPixelConstants;
    ComPtr<ID3D12Resource>                replaySharedConstants;
    ComPtr<ID3D12Resource>                replayTexture;
    ComPtr<ID3D12Resource>                replayTextureUpload;
    ComPtr<ID3D12DescriptorHeap>          replaySamplerHeap;
    ComPtr<ID3D12Resource>                replayInitialSample0;
    ComPtr<ID3D12Resource>                replayInitialSample1;
    ComPtr<ID3D12Resource>                replayInitialSample0Upload;
    ComPtr<ID3D12Resource>                replayInitialSample1Upload;
    std::array<ComPtr<ID3D12Resource>, 4> replayColorTargets;
    ComPtr<ID3D12Resource>                replayResolvedTarget;
    ComPtr<ID3D12Resource>                replayReadback;
    ComPtr<ID3D12DescriptorHeap>          replayRtvHeap;
    ComPtr<ID3D12DescriptorHeap>          replayCopySrvHeap;
    ComPtr<ID3D12Resource>                replayCopyConstants;
    ComPtr<ID3D12Resource>                replayResolveConstants;
    ComPtr<ID3D12Resource>                replayResolveConstantsSample3;
    std::uint64_t                         replayFenceValue         = 0;
    bool                                  replaySubmissionInFlight = false;

    bool waitForFence(std::uint64_t value)
    {
        const auto completedValue = fence->GetCompletedValue();
        if (completedValue == std::numeric_limits<std::uint64_t>::max())
        {
            return logDeviceRemoval("fence status", DXGI_ERROR_DEVICE_REMOVED);
        }
        if (value == 0 || completedValue >= value)
        {
            return true;
        }
        const HRESULT result = fence->SetEventOnCompletion(value, fenceEvent);
        if (FAILED(result))
        {
            return logDeviceRemoval("fence event", result);
        }
        const DWORD waitResult =
            WaitForSingleObject(fenceEvent, kFenceWaitTimeoutMs);
        if (waitResult == WAIT_TIMEOUT)
        {
            REXLOG_ERROR("Native D3D12 fence wait timed out after {} ms",
                         kFenceWaitTimeoutMs);
            const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(reason))
            {
                REXLOG_ERROR("Native D3D12 device removed after fence timeout: HRESULT 0x{:08X}",
                             static_cast<std::uint32_t>(reason));
            }
            return false;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            REXLOG_ERROR("Native D3D12 fence wait failed: Win32 error {}",
                         GetLastError());
            return false;
        }
        const auto completedAfterWait = fence->GetCompletedValue();
        if (completedAfterWait == std::numeric_limits<std::uint64_t>::max())
        {
            return logDeviceRemoval("fence completion", DXGI_ERROR_DEVICE_REMOVED);
        }
        if (completedAfterWait < value)
        {
            REXLOG_ERROR("Native D3D12 fence event fired before value {} completed (completed={})",
                         value,
                         completedAfterWait);
            return false;
        }
        return true;
    }

    bool waitForGpu()
    {
        const std::uint64_t value  = nextFenceValue++;
        const HRESULT       result = queue->Signal(fence.Get(), value);
        if (FAILED(result))
        {
            return logDeviceRemoval("queue signal", result);
        }
        return waitForFence(value);
    }

    void releaseBackBuffers()
    {
        for (auto& buffer : backBuffers)
        {
            buffer.Reset();
        }
    }

    bool createBackBuffers()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE heapStart =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const HRESULT result =
                swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffers[index]));
            if (FAILED(result))
            {
                return logFailure("swap-chain buffer", result);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
            handle.ptr += static_cast<SIZE_T>(index) * rtvStride;
            device->CreateRenderTargetView(backBuffers[index].Get(), nullptr, handle);
        }
        return true;
    }

    bool logDeviceRemoval(const char* operation, HRESULT result)
    {
        REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
        if (FAILED(reason))
        {
            REXLOG_ERROR("Native D3D12 device removed: HRESULT 0x{:08X}",
                         static_cast<std::uint32_t>(reason));
        }
        return false;
    }

    void drainReplayInfoQueue(const char* operation)
    {
        if (!replayInfoQueue)
        {
            return;
        }

        constexpr UINT64 kMaxReportedMessages = 64;
        constexpr SIZE_T kMaxMessageBytes     = 64U * 1024U;
        const UINT64     messageCount         = replayInfoQueue->GetNumStoredMessages();
        const UINT64     reportCount          = std::min(messageCount, kMaxReportedMessages);
        for (UINT64 index = 0; index < reportCount; ++index)
        {
            SIZE_T  messageBytes = 0;
            HRESULT result       = replayInfoQueue->GetMessage(index, nullptr, &messageBytes);
            if (FAILED(result) || messageBytes < sizeof(D3D12_MESSAGE) ||
                messageBytes > kMaxMessageBytes)
            {
                REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] message {} could not be sized: "
                             "HRESULT 0x{:08X}, bytes {}",
                             operation,
                             index,
                             static_cast<std::uint32_t>(result),
                             messageBytes);
                continue;
            }

            std::vector<std::uint8_t> storage(messageBytes);
            auto*                     message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            result                            = replayInfoQueue->GetMessage(index, message, &messageBytes);
            if (FAILED(result))
            {
                REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] message {} could not be read: "
                             "HRESULT 0x{:08X}",
                             operation,
                             index,
                             static_cast<std::uint32_t>(result));
                continue;
            }
            REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] severity={} id={} {}",
                         operation,
                         static_cast<std::uint32_t>(message->Severity),
                         static_cast<std::uint32_t>(message->ID),
                         message->pDescription ? message->pDescription : "<no description>");
        }
        if (messageCount > reportCount)
        {
            REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] omitted {} additional messages",
                         operation,
                         messageCount - reportCount);
        }
        replayInfoQueue->ClearStoredMessages();
    }

    bool                   initializeOnRendererThread(std::uintptr_t nativeWindow,
                                                      std::uint32_t  initialWidth,
                                                      std::uint32_t  initialHeight);
    bool                   initializeDeviceAndCommandObjects(bool replayDiagnostics);
    bool                   initializeHeadlessOnRendererThread(std::uint32_t initialWidth,
                                                              std::uint32_t initialHeight);
    bool                   resizeOnRendererThread(std::uint32_t width, std::uint32_t height);
    bool                   presentOnRendererThread();
    NativeDrawReplayResult executeOffscreenReplayOnRendererThread(
        const NativeDrawReplayRecipe& recipe);
    void detachReplayGpuObjects();
    void shutdownOnRendererThread();
#endif
};

NativeRendererD3D12::NativeRendererD3D12()
: impl(std::make_unique<Impl>())
{
}

NativeRendererD3D12::~NativeRendererD3D12()
{
    Shutdown();
}

#if defined(_WIN32)

bool NativeRendererD3D12::Impl::initializeDeviceAndCommandObjects(bool replayDiagnostics)
{
    if (replayDiagnostics)
    {
        enableReplayDebugLayer();
    }
    enableDred();
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        return logFailure("factory creation", result);
    }

    for (std::uint32_t index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> candidate;
        result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(result))
        {
            return logFailure("adapter enumeration", result);
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        ComPtr<ID3D12Device> candidateDevice;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),
                                        D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&candidateDevice))))
        {
            adapter = std::move(candidate);
            device  = std::move(candidateDevice);
            device->SetName(L"ReRevved native renderer device");
            REXLOG_INFO("Native D3D12 adapter selected: vendor={:04X} device={:04X}",
                        description.VendorId,
                        description.DeviceId);
            break;
        }
    }
    if (!device)
    {
        REXLOG_ERROR("Native D3D12 found no compatible hardware adapter");
        return false;
    }

    replayInfoQueue.Reset();
    if (replayDiagnostics)
    {
        result = device.As(&replayInfoQueue);
        if (FAILED(result))
        {
            REXLOG_WARN("Native D3D12 replay InfoQueue unavailable: HRESULT 0x{:08X}",
                        static_cast<std::uint32_t>(result));
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result         = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        return logFailure("command queue creation", result);
    }
    queue->SetName(L"ReRevved native renderer direct queue");

    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        auto& allocator = allocators[index];
        result          = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result))
        {
            return logFailure("command allocator creation", result);
        }
        allocator->SetName(index == 0 ? L"ReRevved native frame allocator 0"
                                      : L"ReRevved native frame allocator 1");
    }
    result = device->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocators[0].Get(),
                                       nullptr,
                                       IID_PPV_ARGS(&commandList));
    if (FAILED(result))
    {
        return logFailure("command list creation", result);
    }
    commandList->SetName(L"ReRevved native frame command list");
    result = commandList->Close();
    if (FAILED(result))
    {
        return logFailure("initial command list close", result);
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        return logFailure("fence creation", result);
    }
    fence->SetName(L"ReRevved native frame fence");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
    {
        REXLOG_ERROR("Native D3D12 fence event creation failed: Win32 error {}",
                     GetLastError());
        return false;
    }
    return true;
}

bool NativeRendererD3D12::Impl::initializeOnRendererThread(
    std::uintptr_t nativeWindow,
    std::uint32_t  initialWidth,
    std::uint32_t  initialHeight)
{
    const HWND hwnd = reinterpret_cast<HWND>(nativeWindow);
    if (!hwnd || initialWidth == 0 || initialHeight == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable HWND extent");
        return false;
    }
    width  = initialWidth;
    height = initialHeight;

    const auto failInitialization = [this]()
    {
        shutdownOnRendererThread();
        return false;
    };

    if (!initializeDeviceAndCommandObjects(false))
    {
        return failInitialization();
    }

    HRESULT result = S_OK;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width       = width;
    swapChainDesc.Height      = height;
    swapChainDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc  = { 1, 0 };
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> newSwapChain;
    result = factory->CreateSwapChainForHwnd(
        queue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &newSwapChain);
    if (FAILED(result))
    {
        logFailure("swap chain creation", result);
        return failInitialization();
    }
    result = newSwapChain.As(&swapChain);
    if (FAILED(result))
    {
        logFailure("swap chain interface", result);
        return failInitialization();
    }
    result = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result))
    {
        logFailure("window association", result);
        return failInitialization();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    result                 = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(result))
    {
        logFailure("RTV heap creation", result);
        return failInitialization();
    }
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!createBackBuffers())
    {
        return failInitialization();
    }

    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 initialized: {}x{} frames={}", width, height, kFrameCount);
    if (!presentOnRendererThread())
    {
        shutdownOnRendererThread();
        return false;
    }
    REXLOG_INFO("Native D3D12 diagnostic frame presented");
    return true;
}

bool NativeRendererD3D12::Impl::initializeHeadlessOnRendererThread(
    std::uint32_t initialWidth,
    std::uint32_t initialHeight)
{
    if (initialWidth == 0 || initialHeight == 0)
    {
        REXLOG_ERROR("Native D3D12 headless replay has no target extent");
        return false;
    }
    width  = initialWidth;
    height = initialHeight;
    if (!initializeDeviceAndCommandObjects(true))
    {
        shutdownOnRendererThread();
        return false;
    }
    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 headless replay initialized: {}x{}", width, height);
    return true;
}

bool NativeRendererD3D12::Impl::resizeOnRendererThread(std::uint32_t newWidth,
                                                       std::uint32_t newHeight)
{
    if (newWidth == 0 || newHeight == 0 || !initialized.load(std::memory_order_acquire))
    {
        return newWidth == 0 || newHeight == 0;
    }
    if (newWidth == width && newHeight == height)
    {
        return true;
    }
    if (!waitForGpu())
    {
        return false;
    }

    releaseBackBuffers();
    fenceValues.fill(0);
    const HRESULT result = swapChain->ResizeBuffers(
        kFrameCount, newWidth, newHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("swap chain resize", result);
        return false;
    }
    width  = newWidth;
    height = newHeight;
    if (!createBackBuffers())
    {
        return false;
    }
    REXLOG_INFO("Native D3D12 resized: {}x{}", newWidth, newHeight);
    return presentOnRendererThread();
}

bool NativeRendererD3D12::Impl::presentOnRendererThread()
{
    if (!initialized.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::uint32_t frameIndex = swapChain->GetCurrentBackBufferIndex();
    if (!waitForFence(fenceValues[frameIndex]))
    {
        return false;
    }

    HRESULT result = allocators[frameIndex]->Reset();
    if (FAILED(result))
    {
        logFailure("command allocator reset", result);
        return false;
    }
    result = commandList->Reset(allocators[frameIndex].Get(), nullptr);
    if (FAILED(result))
    {
        logFailure("command list reset", result);
        return false;
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = backBuffers[frameIndex].Get();
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex) * rtvStride;
    constexpr float clearColor[4] = { 0.015F, 0.02F, 0.04F, 1.0F };
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &toPresent);

    result = commandList->Close();
    if (FAILED(result))
    {
        logFailure("command list close", result);
        return false;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);

    const std::uint64_t fenceValue = nextFenceValue++;
    result                         = queue->Signal(fence.Get(), fenceValue);
    if (FAILED(result))
    {
        logDeviceRemoval("frame signal", result);
        return false;
    }
    fenceValues[frameIndex] = fenceValue;

    result = swapChain->Present(1, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("present", result);
        return false;
    }
    return true;
}

NativeDrawReplayResult NativeRendererD3D12::Impl::executeOffscreenReplayOnRendererThread(
    const NativeDrawReplayRecipe& recipe)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
    const auto fail   = [&result](std::string message)
    {
        result.error = std::move(message);
        return result;
    };
    if (!initialized.load(std::memory_order_acquire) || !device || !queue || !fence)
    {
        return fail("headless D3D12 device is not initialized");
    }

    replayRootSignature.Reset();
    replayPipelineState.Reset();
    replayCopyRootSignature.Reset();
    replayCopyPipelineState.Reset();
    replayCopyPipelineStateSample3.Reset();
    replayResolvePipelineState.Reset();
    replayVertexBuffer.Reset();
    replayIndexBuffer.Reset();
    replayVertexConstants.Reset();
    replayPixelConstants.Reset();
    replaySharedConstants.Reset();
    replayTexture.Reset();
    replayTextureUpload.Reset();
    replaySamplerHeap.Reset();
    replayInitialSample0.Reset();
    replayInitialSample1.Reset();
    replayInitialSample0Upload.Reset();
    replayInitialSample1Upload.Reset();
    replayResolvedTarget.Reset();
    replayReadback.Reset();
    replayCopyConstants.Reset();
    replayResolveConstants.Reset();
    replayResolveConstantsSample3.Reset();
    replayRtvHeap.Reset();
    replayCopySrvHeap.Reset();
    for (auto& target : replayColorTargets)
    {
        target.Reset();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 5;
    HRESULT hr                 = device->CreateDescriptorHeap(&rtvHeapDesc,
                                                              IID_PPV_ARGS(&replayRtvHeap));
    if (FAILED(hr))
    {
        return fail("could not create replay RTV heap");
    }
    const std::uint32_t replayRtvStride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = recipe.textureMask ? 4 : 3;
    srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr                         = device->CreateDescriptorHeap(&srvHeapDesc,
                                                              IID_PPV_ARGS(&replayCopySrvHeap));
    if (FAILED(hr))
    {
        return fail("could not create replay SRV heap");
    }
    const std::uint32_t replaySrvStride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const auto createTexture = [&](D3D12_RESOURCE_FLAGS     flags,
                                   std::uint32_t            sampleCount,
                                   D3D12_RESOURCE_STATES    initialState,
                                   const D3D12_CLEAR_VALUE* clearValue,
                                   ComPtr<ID3D12Resource>&  resource)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = recipe.width;
        desc.Height           = recipe.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc       = { sampleCount, 0 };
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = flags;
        return device->CreateCommittedResource(&heap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &desc,
                                               initialState,
                                               clearValue,
                                               IID_PPV_ARGS(&resource));
    };
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (std::size_t component = 0; component < 4; ++component)
    {
        clearValue.Color[component] = recipe.clearColor[component] / 255.0F;
    }
    for (auto& target : replayColorTargets)
    {
        hr = createTexture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           recipe.sampleCount,
                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                           &clearValue,
                           target);
        if (FAILED(hr))
        {
            return fail("could not create replay multisample render target");
        }
    }
    hr = createTexture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       1,
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       &clearValue,
                       replayResolvedTarget);
    if (FAILED(hr))
    {
        return fail("could not create replay resolved target");
    }

    const std::uint64_t outputPlaneBytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    const std::uint32_t outputRowPitch = (recipe.width * 4U + 255U) & ~255U;
    const std::uint64_t outputPlaneStride =
        (static_cast<std::uint64_t>(outputRowPitch) * recipe.height + 511ULL) & ~511ULL;
    const std::uint64_t   readbackBytes = outputPlaneStride * 2ULL;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width            = readbackBytes;
    readbackDesc.Height           = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels        = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr                            = device->CreateCommittedResource(&readbackHeap,
                                                                    D3D12_HEAP_FLAG_NONE,
                                                                    &readbackDesc,
                                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    nullptr,
                                                                    IID_PPV_ARGS(&replayReadback));
    if (FAILED(hr))
    {
        return fail("could not create replay readback buffer");
    }

    hr = allocators[0]->Reset();
    if (FAILED(hr))
    {
        return fail("could not reset replay command allocator");
    }
    hr = commandList->Reset(allocators[0].Get(), nullptr);
    if (FAILED(hr))
    {
        return fail("could not reset replay command list");
    }

    const auto makeRowUpload = [&](const std::vector<std::uint8_t>& source,
                                   ComPtr<ID3D12Resource>&          texture,
                                   ComPtr<ID3D12Resource>&          upload)
    {
        const UINT                rowPitch = (recipe.width * 4U + 255U) & ~255U;
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(rowPitch) * recipe.height, 0);
        for (std::uint32_t row = 0; row < recipe.height; ++row)
        {
            std::memcpy(rows.data() + static_cast<std::size_t>(row) * rowPitch,
                        source.data() + static_cast<std::size_t>(row) * recipe.width * 4U,
                        static_cast<std::size_t>(recipe.width) * 4U);
        }
        if (!createReplayUploadBuffer(device.Get(), rows.data(), rows.size(), upload))
        {
            return false;
        }
        hr = createTexture(D3D12_RESOURCE_FLAG_NONE,
                           1,
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           nullptr,
                           texture);
        if (FAILED(hr))
        {
            return false;
        }
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource        = texture.Get();
        destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource                          = upload.Get();
        sourceLocation.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint.Offset             = 0;
        sourceLocation.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        sourceLocation.PlacedFootprint.Footprint.Width    = recipe.width;
        sourceLocation.PlacedFootprint.Footprint.Height   = recipe.height;
        sourceLocation.PlacedFootprint.Footprint.Depth    = 1;
        sourceLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        return true;
    };

    if (!makeRowUpload(recipe.initialSample0,
                       replayInitialSample0,
                       replayInitialSample0Upload) ||
        !makeRowUpload(recipe.initialSample1,
                       replayInitialSample1,
                       replayInitialSample1Upload))
    {
        return fail("could not create replay initial sample textures");
    }

    if (recipe.textureMask)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = recipe.texture.width;
        desc.Height           = recipe.texture.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = mapTextureFormat(recipe.texture.format);
        desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr        = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&replayTexture));
        if (FAILED(hr))
            return fail("could not create replay texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT                               rows       = 0;
        UINT64                             rowBytes   = 0;
        UINT64                             totalBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
        if (rowBytes * rows != recipe.texture.bytes.size() || totalBytes > 128ULL * 1024 * 1024)
            return fail("texture upload footprint does not match the decoded payload");
        std::vector<std::uint8_t> uploadBytes(static_cast<std::size_t>(totalBytes), 0);
        for (UINT row = 0; row < rows; ++row)
        {
            std::memcpy(uploadBytes.data() + footprint.Offset +
                            static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                        recipe.texture.bytes.data() + static_cast<std::size_t>(row) * rowBytes,
                        static_cast<std::size_t>(rowBytes));
        }
        if (!createReplayUploadBuffer(device.Get(), uploadBytes.data(), uploadBytes.size(), replayTextureUpload))
            return fail("could not create replay texture upload");
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource       = replayTextureUpload.Get();
        sourceLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = replayTexture.Get();
        destination.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = replayTexture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = desc.Format;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels     = 1;
        const auto& swizzle         = recipe.texture.swizzle;
        srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
        auto textureHandle = replayCopySrvHeap->GetCPUDescriptorHandleForHeapStart();
        textureHandle.ptr += static_cast<SIZE_T>(3) * replaySrvStride;
        device->CreateShaderResourceView(replayTexture.Get(), &srv, textureHandle);
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
        samplerHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.NumDescriptors = 1;
        samplerHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr                             = device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&replaySamplerHeap));
        if (FAILED(hr))
            return fail("could not create replay sampler heap");
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_ENCODE_BASIC_FILTER(
            recipe.sampler.minLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            recipe.sampler.magLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            recipe.sampler.mipLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        sampler.AddressU       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(recipe.sampler.address[0]);
        sampler.AddressV       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(recipe.sampler.address[1]);
        sampler.AddressW       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(recipe.sampler.address[2]);
        sampler.MipLODBias     = recipe.sampler.mipBias;
        sampler.MinLOD         = recipe.sampler.minLod;
        sampler.MaxLOD         = recipe.sampler.maxLod;
        sampler.MaxAnisotropy  = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        std::copy(recipe.sampler.border.begin(), recipe.sampler.border.end(), sampler.BorderColor);
        device->CreateSampler(&sampler, replaySamplerHeap->GetCPUDescriptorHandleForHeapStart());
    }

    const auto makeConstantBuffer = [&](const std::vector<std::uint8_t>& bytes,
                                        ComPtr<ID3D12Resource>&          resource)
    {
        const std::uint32_t       size = alignConstantBytes(bytes.size());
        std::vector<std::uint8_t> padded(size, 0);
        std::memcpy(padded.data(), bytes.data(), bytes.size());
        return createReplayUploadBuffer(device.Get(), padded.data(), padded.size(), resource);
    };
    if (!makeConstantBuffer(recipe.vertexConstants, replayVertexConstants) ||
        !makeConstantBuffer(recipe.pixelConstants, replayPixelConstants) ||
        !makeConstantBuffer(recipe.sharedConstants, replaySharedConstants))
    {
        return fail("could not create replay stage constant buffers");
    }
    if (!createReplayUploadBuffer(device.Get(),
                                  recipe.vertexData.data(),
                                  recipe.vertexData.size(),
                                  replayVertexBuffer))
    {
        return fail("could not create replay vertex buffer");
    }
    std::vector<std::uint8_t> indexBytes(recipe.indices.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0; index < recipe.indices.size(); ++index)
    {
        const std::uint32_t value = recipe.indices[index];
        indexBytes[index * 4 + 0] = static_cast<std::uint8_t>(value);
        indexBytes[index * 4 + 1] = static_cast<std::uint8_t>(value >> 8);
        indexBytes[index * 4 + 2] = static_cast<std::uint8_t>(value >> 16);
        indexBytes[index * 4 + 3] = static_cast<std::uint8_t>(value >> 24);
    }
    if (!createReplayUploadBuffer(device.Get(),
                                  indexBytes.data(),
                                  indexBytes.size(),
                                  replayIndexBuffer))
    {
        return fail("could not create replay index buffer");
    }

    struct CopyConstants
    {
        std::uint32_t sampleA;
        std::uint32_t sampleB;
        std::uint32_t sampleCount;
        std::uint32_t padding;
    };

    const CopyConstants           initConstants{ 0, 0, 0, 0 };
    const CopyConstants           resolveConstants{ 0, 0, 0, 0 };
    const CopyConstants           resolveSample3Constants{ 3, 3, 0, 0 };
    std::array<std::uint8_t, 256> initConstantBytes{};
    std::array<std::uint8_t, 256> resolveConstantBytes{};
    std::array<std::uint8_t, 256> resolveSample3ConstantBytes{};
    std::memcpy(initConstantBytes.data(), &initConstants, sizeof(initConstants));
    std::memcpy(resolveConstantBytes.data(), &resolveConstants, sizeof(resolveConstants));
    std::memcpy(resolveSample3ConstantBytes.data(),
                &resolveSample3Constants,
                sizeof(resolveSample3Constants));
    if (!createReplayUploadBuffer(device.Get(),
                                  initConstantBytes.data(),
                                  initConstantBytes.size(),
                                  replayCopyConstants) ||
        !createReplayUploadBuffer(device.Get(),
                                  resolveConstantBytes.data(),
                                  resolveConstantBytes.size(),
                                  replayResolveConstants) ||
        !createReplayUploadBuffer(device.Get(),
                                  resolveSample3ConstantBytes.data(),
                                  resolveSample3ConstantBytes.size(),
                                  replayResolveConstantsSample3))
    {
        return fail("could not create replay copy constants");
    }

    D3D12_DESCRIPTOR_RANGE1 copyRange{};
    copyRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    copyRange.NumDescriptors     = 1;
    copyRange.BaseShaderRegister = 0;
    copyRange.RegisterSpace      = 0;
    copyRange.Flags              = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_ROOT_PARAMETER1 copyParameters[2]{};
    copyParameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    copyParameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    copyParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    copyParameters[0].DescriptorTable.pDescriptorRanges   = &copyRange;
    copyParameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
    copyParameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    copyParameters[1].Descriptor.ShaderRegister           = 0;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copyRootDesc{};
    copyRootDesc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    copyRootDesc.Desc_1_1.NumParameters = 2;
    copyRootDesc.Desc_1_1.pParameters   = copyParameters;
    copyRootDesc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> serializedRoot;
    ComPtr<ID3DBlob> rootErrors;
    hr = D3D12SerializeVersionedRootSignature(&copyRootDesc,
                                              &serializedRoot,
                                              &rootErrors);
    if (FAILED(hr))
    {
        return fail("could not serialize replay copy root signature");
    }
    hr = device->CreateRootSignature(0,
                                     serializedRoot->GetBufferPointer(),
                                     serializedRoot->GetBufferSize(),
                                     IID_PPV_ARGS(&replayCopyRootSignature));
    if (FAILED(hr))
    {
        return fail("could not create replay copy root signature");
    }

    ComPtr<ID3DBlob> copyVertexShader;
    ComPtr<ID3DBlob> copyPixelShader;
    ComPtr<ID3DBlob> resolvePixelShader;
    if (!compileReplayShader(kReplayCopyShaderSource,
                             "copy_vs",
                             "vs_5_1",
                             copyVertexShader) ||
        !compileReplayShader(kReplayCopyShaderSource,
                             "copy_ps",
                             "ps_5_1",
                             copyPixelShader) ||
        !compileReplayShader(kReplayResolveShaderSource,
                             "copy_ps",
                             "ps_5_1",
                             resolvePixelShader))
    {
        return fail("could not compile replay copy helper shaders");
    }

    D3D12_BLEND_DESC copyBlend{};
    for (auto& targetBlend : copyBlend.RenderTarget)
    {
        targetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    D3D12_RASTERIZER_DESC copyRaster{};
    copyRaster.FillMode          = D3D12_FILL_MODE_SOLID;
    copyRaster.CullMode          = D3D12_CULL_MODE_NONE;
    copyRaster.DepthClipEnable   = TRUE;
    copyRaster.MultisampleEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC copyDepth{};
    copyDepth.DepthEnable   = FALSE;
    copyDepth.StencilEnable = FALSE;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copyPso{};
    copyPso.pRootSignature        = replayCopyRootSignature.Get();
    copyPso.VS                    = { copyVertexShader->GetBufferPointer(), copyVertexShader->GetBufferSize() };
    copyPso.PS                    = { copyPixelShader->GetBufferPointer(), copyPixelShader->GetBufferSize() };
    copyPso.BlendState            = copyBlend;
    copyPso.RasterizerState       = copyRaster;
    copyPso.DepthStencilState     = copyDepth;
    copyPso.SampleMask            = 1;
    copyPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    copyPso.NumRenderTargets      = 4;
    copyPso.SampleDesc            = { recipe.sampleCount, 0 };
    for (auto& format : copyPso.RTVFormats)
    {
        format = DXGI_FORMAT_UNKNOWN;
    }
    for (std::size_t index = 0; index < 4; ++index)
    {
        copyPso.RTVFormats[index] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    hr = device->CreateGraphicsPipelineState(&copyPso,
                                             IID_PPV_ARGS(&replayCopyPipelineState));
    if (FAILED(hr))
    {
        return fail("could not create replay initial-sample pipeline");
    }
    copyPso.SampleMask = 1U << 3;
    hr                 = device->CreateGraphicsPipelineState(&copyPso,
                                                             IID_PPV_ARGS(&replayCopyPipelineStateSample3));
    if (FAILED(hr))
    {
        return fail("could not create replay sample-3 pipeline");
    }

    copyPso.PS               = { resolvePixelShader->GetBufferPointer(), resolvePixelShader->GetBufferSize() };
    copyPso.SampleMask       = D3D12_DEFAULT_SAMPLE_MASK;
    copyPso.NumRenderTargets = 1;
    copyPso.SampleDesc       = { 1, 0 };
    copyPso.RTVFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (std::size_t index = 1; index < std::size(copyPso.RTVFormats); ++index)
    {
        copyPso.RTVFormats[index] = DXGI_FORMAT_UNKNOWN;
    }
    hr = device->CreateGraphicsPipelineState(&copyPso,
                                             IID_PPV_ARGS(&replayResolvePipelineState));
    if (FAILED(hr))
    {
        return fail("could not create replay resolve pipeline");
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart =
        replayRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::size_t index = 0; index < replayColorTargets.size(); ++index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvStart;
        handle.ptr += static_cast<SIZE_T>(index) * replayRtvStride;
        device->CreateRenderTargetView(replayColorTargets[index].Get(), nullptr, handle);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE resolvedRtv = rtvStart;
    resolvedRtv.ptr += static_cast<SIZE_T>(4) * replayRtvStride;
    device->CreateRenderTargetView(replayResolvedTarget.Get(), nullptr, resolvedRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC initialSrv{};
    initialSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    initialSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    initialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    initialSrv.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvStart =
        replayCopySrvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(replayInitialSample0.Get(), &initialSrv, srvStart);
    D3D12_CPU_DESCRIPTOR_HANDLE sample1Srv = srvStart;
    sample1Srv.ptr += static_cast<SIZE_T>(replaySrvStride);
    device->CreateShaderResourceView(replayInitialSample1.Get(), &initialSrv, sample1Srv);
    D3D12_SHADER_RESOURCE_VIEW_DESC msaaSrv{};
    msaaSrv.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaaSrv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    msaaSrv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    D3D12_CPU_DESCRIPTOR_HANDLE targetSrv = srvStart;
    targetSrv.ptr += static_cast<SIZE_T>(2) * replaySrvStride;
    device->CreateShaderResourceView(replayColorTargets[0].Get(), &msaaSrv, targetSrv);

    const auto transitionTexture = [&](ID3D12Resource*       resource,
                                       D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter  = after;
        commandList->ResourceBarrier(1, &barrier);
    };
    transitionTexture(replayInitialSample0.Get(),
                      D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTexture(replayInitialSample1.Get(),
                      D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT fullTargetViewport{ 0.0F,
                                       0.0F,
                                       static_cast<float>(recipe.width),
                                       static_cast<float>(recipe.height),
                                       0.0F,
                                       1.0F };
    D3D12_RECT     fullTargetScissor{ 0,
                                      0,
                                      static_cast<LONG>(recipe.width),
                                      static_cast<LONG>(recipe.height) };
    D3D12_VIEWPORT replayViewport{ recipe.viewport.x,
                                   recipe.viewport.y,
                                   recipe.viewport.width,
                                   recipe.viewport.height,
                                   recipe.viewport.minDepth,
                                   recipe.viewport.maxDepth };
    D3D12_RECT     replayScissor{ recipe.scissor.left,
                                  recipe.scissor.top,
                                  recipe.scissor.right,
                                  recipe.scissor.bottom };
    commandList->RSSetViewports(1, &fullTargetViewport);
    commandList->RSSetScissorRects(1, &fullTargetScissor);
    commandList->SetGraphicsRootSignature(replayCopyRootSignature.Get());
    ID3D12DescriptorHeap* descriptorHeaps[] = { replayCopySrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    D3D12_CPU_DESCRIPTOR_HANDLE targetRtvs[4];
    for (std::size_t index = 0; index < 4; ++index)
    {
        targetRtvs[index] = rtvStart;
        targetRtvs[index].ptr += static_cast<SIZE_T>(index) * replayRtvStride;
        commandList->ClearRenderTargetView(targetRtvs[index], clearValue.Color, 0, nullptr);
    }
    commandList->SetPipelineState(replayCopyPipelineState.Get());
    commandList->SetGraphicsRootDescriptorTable(0, replayCopySrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootConstantBufferView(1, replayCopyConstants->GetGPUVirtualAddress());
    commandList->OMSetRenderTargets(4, targetRtvs, FALSE, nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE sample1Gpu = replayCopySrvHeap->GetGPUDescriptorHandleForHeapStart();
    sample1Gpu.ptr += static_cast<UINT64>(replaySrvStride);
    commandList->SetPipelineState(replayCopyPipelineStateSample3.Get());
    commandList->SetGraphicsRootDescriptorTable(0, sample1Gpu);
    commandList->DrawInstanced(3, 1, 0, 0);

    D3D12_ROOT_PARAMETER1 drawParameters[5]{};
    drawParameters[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    drawParameters[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
    drawParameters[0].Descriptor.ShaderRegister = 0;
    drawParameters[0].Descriptor.RegisterSpace  = 4;
    drawParameters[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    drawParameters[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    drawParameters[1].Descriptor.ShaderRegister = 1;
    drawParameters[1].Descriptor.RegisterSpace  = 4;
    drawParameters[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    drawParameters[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    drawParameters[2].Descriptor.ShaderRegister = 2;
    drawParameters[2].Descriptor.RegisterSpace  = 4;
    D3D12_DESCRIPTOR_RANGE1 textureRange{};
    textureRange.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.Flags          = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_DESCRIPTOR_RANGE1 samplerRange{};
    samplerRange.RangeType             = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors        = 1;
    samplerRange.RegisterSpace         = 3;
    drawParameters[3].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    drawParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    drawParameters[3].DescriptorTable  = { 1, &textureRange };
    drawParameters[4].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    drawParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    drawParameters[4].DescriptorTable  = { 1, &samplerRange };
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC drawRootDesc{};
    drawRootDesc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    drawRootDesc.Desc_1_1.NumParameters = recipe.textureMask ? 5 : 3;
    drawRootDesc.Desc_1_1.pParameters   = drawParameters;
    drawRootDesc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serializedRoot.Reset();
    rootErrors.Reset();
    hr = D3D12SerializeVersionedRootSignature(&drawRootDesc,
                                              &serializedRoot,
                                              &rootErrors);
    if (FAILED(hr))
    {
        return fail("could not serialize replay draw root signature");
    }
    hr = device->CreateRootSignature(0,
                                     serializedRoot->GetBufferPointer(),
                                     serializedRoot->GetBufferSize(),
                                     IID_PPV_ARGS(&replayRootSignature));
    if (FAILED(hr))
    {
        return fail("could not create replay draw root signature");
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements(recipe.vertexAttributeCount);
    for (std::uint32_t attribute = 0; attribute < recipe.vertexAttributeCount; ++attribute)
    {
        inputElements[attribute] = { "TEXCOORD", attribute, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, attribute * 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    }
    D3D12_BLEND_DESC drawBlend{};
    drawBlend.AlphaToCoverageEnable  = recipe.blend.alphaToCoverage ? TRUE : FALSE;
    drawBlend.IndependentBlendEnable = FALSE;
    for (auto& targetBlend : drawBlend.RenderTarget)
    {
        targetBlend.BlendEnable           = recipe.blend.enabled ? TRUE : FALSE;
        targetBlend.SrcBlend              = mapBlendFactor(recipe.blend.sourceColor);
        targetBlend.DestBlend             = mapBlendFactor(recipe.blend.destinationColor);
        targetBlend.BlendOp               = mapBlendOp(recipe.blend.colorOp);
        targetBlend.SrcBlendAlpha         = mapBlendFactor(recipe.blend.sourceAlpha);
        targetBlend.DestBlendAlpha        = mapBlendFactor(recipe.blend.destinationAlpha);
        targetBlend.BlendOpAlpha          = mapBlendOp(recipe.blend.alphaOp);
        targetBlend.RenderTargetWriteMask = recipe.blend.writeMask;
    }
    D3D12_RASTERIZER_DESC drawRaster{};
    drawRaster.FillMode              = D3D12_FILL_MODE_SOLID;
    drawRaster.CullMode              = D3D12_CULL_MODE_NONE;
    drawRaster.FrontCounterClockwise = FALSE;
    drawRaster.DepthClipEnable       = TRUE;
    drawRaster.MultisampleEnable     = TRUE;
    D3D12_DEPTH_STENCIL_DESC drawDepth{};
    drawDepth.DepthEnable   = FALSE;
    drawDepth.StencilEnable = FALSE;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawPso{};
    drawPso.pRootSignature        = replayRootSignature.Get();
    drawPso.VS                    = { recipe.vertexShaderDxil.data(), recipe.vertexShaderDxil.size() };
    drawPso.PS                    = { recipe.pixelShaderDxil.data(), recipe.pixelShaderDxil.size() };
    drawPso.BlendState            = drawBlend;
    drawPso.SampleMask            = recipe.sampleMask;
    drawPso.RasterizerState       = drawRaster;
    drawPso.DepthStencilState     = drawDepth;
    drawPso.InputLayout           = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
    drawPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    drawPso.NumRenderTargets      = 4;
    drawPso.SampleDesc            = { recipe.sampleCount, 0 };
    for (auto& format : drawPso.RTVFormats)
    {
        format = DXGI_FORMAT_UNKNOWN;
    }
    for (std::size_t index = 0; index < 4; ++index)
    {
        drawPso.RTVFormats[index] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (replayInfoQueue)
    {
        replayInfoQueue->ClearStoredMessages();
    }
    hr = device->CreateGraphicsPipelineState(&drawPso,
                                             IID_PPV_ARGS(&replayPipelineState));
    if (FAILED(hr))
    {
        REXLOG_ERROR("Native D3D12 replay guest-DXIL pipeline creation failed: "
                     "HRESULT 0x{:08X}",
                     static_cast<std::uint32_t>(hr));
        drainReplayInfoQueue("guest-DXIL PSO");
        return fail("could not create replay guest-DXIL pipeline");
    }
    commandList->RSSetViewports(1, &replayViewport);
    commandList->RSSetScissorRects(1, &replayScissor);
    commandList->SetGraphicsRootSignature(replayRootSignature.Get());
    commandList->SetPipelineState(replayPipelineState.Get());
    commandList->OMSetRenderTargets(4, targetRtvs, FALSE, nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    vertexView.BufferLocation = replayVertexBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes    = static_cast<UINT>(recipe.vertexData.size());
    vertexView.StrideInBytes  = recipe.vertexStrideBytes;
    commandList->IASetVertexBuffers(0, 1, &vertexView);
    D3D12_INDEX_BUFFER_VIEW indexView{};
    indexView.BufferLocation = replayIndexBuffer->GetGPUVirtualAddress();
    indexView.SizeInBytes    = static_cast<UINT>(indexBytes.size());
    indexView.Format         = DXGI_FORMAT_R32_UINT;
    commandList->IASetIndexBuffer(&indexView);
    commandList->SetGraphicsRootConstantBufferView(0, replayVertexConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, replayPixelConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, replaySharedConstants->GetGPUVirtualAddress());
    if (recipe.textureMask)
    {
        ID3D12DescriptorHeap* drawHeaps[] = { replayCopySrvHeap.Get(), replaySamplerHeap.Get() };
        commandList->SetDescriptorHeaps(2, drawHeaps);
        auto textureHandle = replayCopySrvHeap->GetGPUDescriptorHandleForHeapStart();
        textureHandle.ptr += static_cast<UINT64>(3) * replaySrvStride;
        commandList->SetGraphicsRootDescriptorTable(3, textureHandle);
        commandList->SetGraphicsRootDescriptorTable(4, replaySamplerHeap->GetGPUDescriptorHandleForHeapStart());
    }
    commandList->DrawIndexedInstanced(recipe.indexCount, 1, 0, 0, 0);

    for (auto& target : replayColorTargets)
    {
        transitionTexture(target.Get(),
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    commandList->RSSetViewports(1, &fullTargetViewport);
    commandList->RSSetScissorRects(1, &fullTargetScissor);
    commandList->SetGraphicsRootSignature(replayCopyRootSignature.Get());
    commandList->SetPipelineState(replayResolvePipelineState.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE targetSrvGpu =
        replayCopySrvHeap->GetGPUDescriptorHandleForHeapStart();
    targetSrvGpu.ptr += static_cast<UINT64>(2) * replaySrvStride;
    commandList->SetGraphicsRootDescriptorTable(0, targetSrvGpu);
    commandList->OMSetRenderTargets(1, &resolvedRtv, FALSE, nullptr);
    D3D12_TEXTURE_COPY_LOCATION readbackLocation{};
    readbackLocation.pResource                          = replayReadback.Get();
    readbackLocation.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readbackLocation.PlacedFootprint.Offset             = 0;
    readbackLocation.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    readbackLocation.PlacedFootprint.Footprint.Width    = recipe.width;
    readbackLocation.PlacedFootprint.Footprint.Height   = recipe.height;
    readbackLocation.PlacedFootprint.Footprint.Depth    = 1;
    readbackLocation.PlacedFootprint.Footprint.RowPitch = outputRowPitch;
    D3D12_TEXTURE_COPY_LOCATION resolvedLocation{};
    resolvedLocation.pResource        = replayResolvedTarget.Get();
    resolvedLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    resolvedLocation.SubresourceIndex = 0;
    const auto recordSampleReadback   = [&](ID3D12Resource* constants,
                                            std::uint64_t   outputOffset)
    {
        commandList->SetGraphicsRootConstantBufferView(1, constants->GetGPUVirtualAddress());
        commandList->DrawInstanced(3, 1, 0, 0);
        transitionTexture(replayResolvedTarget.Get(),
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
        readbackLocation.PlacedFootprint.Offset = outputOffset;
        commandList->CopyTextureRegion(&readbackLocation,
                                       0,
                                       0,
                                       0,
                                       &resolvedLocation,
                                       nullptr);
        if (outputOffset == 0)
        {
            transitionTexture(replayResolvedTarget.Get(),
                              D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    };
    recordSampleReadback(replayResolveConstants.Get(), 0);
    recordSampleReadback(replayResolveConstantsSample3.Get(), outputPlaneStride);
    hr = commandList->Close();
    if (FAILED(hr))
    {
        return fail("could not close replay command list");
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);
    replayFenceValue         = nextFenceValue++;
    hr                       = queue->Signal(fence.Get(), replayFenceValue);
    replaySubmissionInFlight = SUCCEEDED(hr);
    if (FAILED(hr))
    {
        logDeviceRemoval("replay signal", hr);
        return fail("replay queue signal failed");
    }
    if (!waitForFence(replayFenceValue))
    {
        return fail("replay fence did not complete");
    }
    replaySubmissionInFlight = false;

    std::vector<std::uint8_t> output(static_cast<std::size_t>(outputPlaneBytes * 2ULL));
    D3D12_RANGE               readRange{ 0, static_cast<SIZE_T>(readbackBytes) };
    void*                     mapped = nullptr;
    hr                               = replayReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr))
    {
        return fail("could not map replay readback");
    }
    for (std::size_t plane = 0; plane < 2; ++plane)
    {
        for (std::uint32_t row = 0; row < recipe.height; ++row)
        {
            std::memcpy(output.data() + plane * static_cast<std::size_t>(outputPlaneBytes) +
                            static_cast<std::size_t>(row) * recipe.width * 4U,
                        static_cast<const std::uint8_t*>(mapped) + plane * outputPlaneStride +
                            static_cast<std::size_t>(row) * outputRowPitch,
                        static_cast<std::size_t>(recipe.width) * 4U);
        }
    }
    D3D12_RANGE writtenRange{ 0, 0 };
    replayReadback->Unmap(0, &writtenRange);

    std::ofstream outputFile(recipe.outputPath, std::ios::binary | std::ios::trunc);
    if (!outputFile)
    {
        return fail("could not open replay output file");
    }
    outputFile.write(reinterpret_cast<const char*>(output.data()),
                     static_cast<std::streamsize>(output.size()));
    outputFile.flush();
    outputFile.close();
    if (!outputFile)
    {
        return fail("could not flush or close replay output file");
    }
    result.success = true;
    return result;
}

void NativeRendererD3D12::Impl::detachReplayGpuObjects()
{
    (void)replayRootSignature.Detach();
    (void)replayPipelineState.Detach();
    (void)replayCopyRootSignature.Detach();
    (void)replayCopyPipelineState.Detach();
    (void)replayCopyPipelineStateSample3.Detach();
    (void)replayResolvePipelineState.Detach();
    (void)replayVertexBuffer.Detach();
    (void)replayIndexBuffer.Detach();
    (void)replayVertexConstants.Detach();
    (void)replayPixelConstants.Detach();
    (void)replaySharedConstants.Detach();
    (void)replayTexture.Detach();
    (void)replayTextureUpload.Detach();
    (void)replaySamplerHeap.Detach();
    (void)replayInitialSample0.Detach();
    (void)replayInitialSample1.Detach();
    (void)replayInitialSample0Upload.Detach();
    (void)replayInitialSample1Upload.Detach();
    for (auto& target : replayColorTargets)
    {
        (void)target.Detach();
    }
    (void)replayResolvedTarget.Detach();
    (void)replayReadback.Detach();
    (void)replayRtvHeap.Detach();
    (void)replayCopySrvHeap.Detach();
    (void)replayCopyConstants.Detach();
    (void)replayResolveConstants.Detach();
    (void)replayResolveConstantsSample3.Detach();
}

void NativeRendererD3D12::Impl::shutdownOnRendererThread()
{
    bool abandonGpuObjects = false;
    if (initialized.load(std::memory_order_acquire) && queue && fence && !waitForGpu())
    {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
        abandonGpuObjects    = !FAILED(reason);
        if (abandonGpuObjects)
        {
            // A live device with an uncompleted fence may still reference every
            // submitted object. Let process teardown reclaim them instead of
            // releasing storage that the GPU may still be using.
            REXLOG_ERROR("Native D3D12 drain failed on a live device; abandoning GPU objects until process exit");
        }
    }
    initialized.store(false, std::memory_order_release);
    if (abandonGpuObjects)
    {
        detachReplayGpuObjects();
        (void)commandList.Detach();
        for (auto& allocator : allocators)
        {
            (void)allocator.Detach();
        }
        for (auto& backBuffer : backBuffers)
        {
            (void)backBuffer.Detach();
        }
        (void)rtvHeap.Detach();
        (void)swapChain.Detach();
        (void)queue.Detach();
        (void)fence.Detach();
        (void)replayInfoQueue.Detach();
        (void)device.Detach();
        (void)adapter.Detach();
        (void)factory.Detach();
        fenceEvent = nullptr;
        gpuObjectsAbandoned.store(true, std::memory_order_release);
        return;
    }
    commandList.Reset();
    for (auto& allocator : allocators)
    {
        allocator.Reset();
    }
    releaseBackBuffers();
    replayRootSignature.Reset();
    replayPipelineState.Reset();
    replayCopyRootSignature.Reset();
    replayCopyPipelineState.Reset();
    replayCopyPipelineStateSample3.Reset();
    replayResolvePipelineState.Reset();
    replayVertexBuffer.Reset();
    replayIndexBuffer.Reset();
    replayVertexConstants.Reset();
    replayPixelConstants.Reset();
    replaySharedConstants.Reset();
    replayTexture.Reset();
    replayTextureUpload.Reset();
    replaySamplerHeap.Reset();
    replayInitialSample0.Reset();
    replayInitialSample1.Reset();
    replayInitialSample0Upload.Reset();
    replayInitialSample1Upload.Reset();
    for (auto& target : replayColorTargets)
    {
        target.Reset();
    }
    replayResolvedTarget.Reset();
    replayReadback.Reset();
    replayRtvHeap.Reset();
    replayCopySrvHeap.Reset();
    replayCopyConstants.Reset();
    replayResolveConstants.Reset();
    replayResolveConstantsSample3.Reset();
    replayFenceValue         = 0;
    replaySubmissionInFlight = false;
    rtvHeap.Reset();
    swapChain.Reset();
    queue.Reset();
    fence.Reset();
    replayInfoQueue.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
    if (fenceEvent)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    fenceValues.fill(0);
    nextFenceValue = 1;
    width          = 0;
    height         = 0;
}

#endif

void NativeRendererD3D12::rendererThreadMain(std::uintptr_t nativeWindow,
                                             std::uint32_t  width,
                                             std::uint32_t  height)
{
#if defined(_WIN32)
    const bool initializationSucceeded =
        impl->initializeOnRendererThread(nativeWindow, width, height);
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = initializationSucceeded;
        impl->initializationDone      = true;
    }
    impl->stateCv.notify_all();
    if (!initializationSucceeded)
    {
        return;
    }

    for (;;)
    {
        bool          resize          = false;
        std::uint32_t requestedWidth  = 0;
        std::uint32_t requestedHeight = 0;
        {
            std::unique_lock lock(impl->stateMutex);
            impl->stateCv.wait(lock, [this]()
                               {
                                   return impl->stopRequested || impl->resizePending;
                               });
            if (impl->stopRequested)
            {
                break;
            }
            resize              = impl->resizePending;
            requestedWidth      = impl->requestedWidth;
            requestedHeight     = impl->requestedHeight;
            impl->resizePending = false;
        }

        bool requestSucceeded = true;
        if (resize)
        {
            requestSucceeded = impl->resizeOnRendererThread(
                requestedWidth, requestedHeight);
        }
        if (!requestSucceeded)
        {
            handleRendererFailure();
            break;
        }
    }
    impl->shutdownOnRendererThread();
#else
    (void)nativeWindow;
    (void)width;
    (void)height;
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = false;
        impl->initializationDone      = true;
    }
    impl->stateCv.notify_all();
#endif
    impl->initialized.store(false, std::memory_order_release);
}

void NativeRendererD3D12::headlessReplayThreadMain(
    NativeDrawReplayRecipe               recipe,
    std::promise<NativeDrawReplayResult> resultPromise)
{
#if defined(_WIN32)
    NativeDrawReplayResult result;
    try
    {
        if (!impl->initializeHeadlessOnRendererThread(recipe.width, recipe.height))
        {
            result.outputPath = recipe.outputPath;
            result.error      = "headless D3D12 initialization failed";
        }
        else
        {
            result = impl->executeOffscreenReplayOnRendererThread(recipe);
        }
    }
    catch (const std::exception& exception)
    {
        result.outputPath = recipe.outputPath;
        result.error      = std::string("native replay exception: ") + exception.what();
    }
    catch (...)
    {
        result.outputPath = recipe.outputPath;
        result.error      = "native replay failed with an unknown exception";
    }
    impl->shutdownOnRendererThread();
    resultPromise.set_value(std::move(result));
#else
    (void)recipe;
    NativeDrawReplayResult result;
    result.error = "native D3D12 replay requires Windows";
    resultPromise.set_value(std::move(result));
#endif
}

void NativeRendererD3D12::handleRendererFailure()
{
    std::function<void()> requestQuit;
    {
        std::lock_guard lock(impl->stateMutex);
        if (impl->runtimeFailed)
        {
            return;
        }
        impl->runtimeFailed = true;
        impl->stopRequested = true;
        requestQuit         = impl->requestDeferredQuit;
    }
    REXLOG_ERROR("Native D3D12 renderer entered terminal failure; requesting deferred quit");
    impl->stateCv.notify_all();
    if (requestQuit)
    {
        requestQuit();
    }
}

bool NativeRendererD3D12::Initialize(rex::ui::Window& window)
{
#if !defined(_WIN32)
    (void)window;
    REXLOG_ERROR("The native renderer currently requires Windows D3D12");
    return false;
#else
    if (impl->initialized.load(std::memory_order_acquire))
    {
        return true;
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        REXLOG_ERROR("Native D3D12 cannot reinitialize after abandoning in-flight GPU objects");
        return false;
    }
    if (impl->rendererThread.joinable())
    {
        impl->rendererThread.join();
    }

    const HWND hwnd   = static_cast<HWND>(window.GetNativeWindowHandle());
    const auto width  = window.GetActualPhysicalWidth();
    const auto height = window.GetActualPhysicalHeight();
    if (!hwnd)
    {
        REXLOG_ERROR("Native D3D12 could not acquire the ReRevved HWND surface");
        return false;
    }
    if (width == 0 || height == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable pixel extent");
        return false;
    }

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    ResetGuestSwapCorrelation();
    auto* appContext = &window.app_context();
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested           = false;
        impl->initializationDone      = false;
        impl->initializationSucceeded = false;
        impl->resizePending           = false;
        impl->requestedWidth          = width;
        impl->requestedHeight         = height;
        impl->runtimeFailed           = false;
        impl->requestDeferredQuit     = [appContext]()
        {
            appContext->RequestDeferredQuit();
        };
    }

    impl->rendererThread = std::thread(
        &NativeRendererD3D12::rendererThreadMain,
        this,
        reinterpret_cast<std::uintptr_t>(hwnd),
        width,
        height);
    {
        std::unique_lock lock(impl->stateMutex);
        impl->stateCv.wait(lock, [this]()
                           {
                               return impl->initializationDone;
                           });
        if (!impl->initializationSucceeded)
        {
            lock.unlock();
            impl->rendererThread.join();
            return false;
        }
    }
    return true;
#endif
}

NativeDrawReplayResult NativeRendererD3D12::ReplayOffscreen(
    const NativeDrawReplayRecipe& recipe)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
#if !defined(_WIN32)
    result.error = "native D3D12 replay requires Windows";
    return result;
#else
    std::string validationError;
    if (!ValidateNativeDrawReplayRecipe(recipe, validationError))
    {
        result.error = std::move(validationError);
        return result;
    }
    if (recipe.outputPath.empty())
    {
        result.error = "native D3D12 replay output path must be supplied by the CLI";
        return result;
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        result.error = "native D3D12 cannot replay after abandoning in-flight GPU objects";
        return result;
    }
    if (impl->rendererThread.joinable())
    {
        result.error = "native D3D12 renderer thread is already running";
        return result;
    }
    std::promise<NativeDrawReplayResult> resultPromise;
    auto                                 resultFuture = resultPromise.get_future();
    impl->rendererThread                              = std::thread(
        &NativeRendererD3D12::headlessReplayThreadMain,
        this,
        recipe,
        std::move(resultPromise));
    result = resultFuture.get();
    impl->rendererThread.join();
    return result;
#endif
}

bool NativeRendererD3D12::Resize(std::uint32_t width, std::uint32_t height)
{
    // A minimized window has no drawable extent. Keep the last valid request
    // and let the next non-zero callback wake the renderer.
    if (width == 0 || height == 0)
    {
        return true;
    }
    std::lock_guard lock(impl->stateMutex);
    if (!impl->initialized.load(std::memory_order_acquire) ||
        impl->stopRequested || impl->runtimeFailed)
    {
        return false;
    }

    // The UI thread only updates this mailbox; the renderer consumes the
    // latest dimensions and performs all DXGI/D3D12 work itself.
    impl->requestedWidth  = width;
    impl->requestedHeight = height;
    impl->resizePending   = true;
    impl->stateCv.notify_one();
    return true;
}

void NativeRendererD3D12::Shutdown()
{
    if (!impl->rendererThread.joinable())
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initialized.store(false, std::memory_order_release);
        impl->requestDeferredQuit = {};
        impl->stateCv.notify_all();
        return;
    }
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested = true;
        impl->resizePending = false;
    }
    impl->stateCv.notify_all();
    if (impl->rendererThread.get_id() != std::this_thread::get_id())
    {
        impl->rendererThread.join();
    }
    std::lock_guard lock(impl->stateMutex);
    impl->requestDeferredQuit = {};
}

bool NativeRendererD3D12::Initialized() const noexcept
{
    return impl->initialized.load(std::memory_order_acquire);
}

} // namespace rerevved::gpu
