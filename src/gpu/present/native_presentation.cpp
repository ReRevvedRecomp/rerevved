#include "native_presentation.h"
#include "native_immediate_drawer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include <d3d12.h>
#include <plume_d3d12.h>
#include <rex/logging.h>
#include <wrl/client.h>

#include "offer_gate.h"
#include "plume_host.h"

#include "native_present_pixel.h"
#include "native_present_vertex.h"

namespace rerevved::gpu
{

namespace
{

using Microsoft::WRL::ComPtr;

constexpr std::size_t   kCopySlot           = 0;
constexpr std::size_t   kFirstPresentSlot   = 1;
constexpr std::uint32_t kPresentSlotCount   = 2;
constexpr std::uint32_t kWorkSlotCount      = 1 + kPresentSlotCount;
constexpr UINT64        kMaxSourceDimension = 8192;
constexpr DXGI_FORMAT   kDisplayFormat      = DXGI_FORMAT_R10G10B10A2_UNORM;

// Owners that the GPU may still reference after an unproved drain live here
// until the process exits.
void retainForProcessLifetime(std::shared_ptr<const void> owner)
{
    static auto*      retained = new std::vector<std::shared_ptr<const void>>();
    static std::mutex mutex;
    std::lock_guard   lock(mutex);
    retained->push_back(std::move(owner));
}

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

HostResult hostFailed(const char* operation, std::int32_t nativeCode = 0) noexcept
{
    HostResult result;
    result.status     = HostStatus::Failed;
    result.nativeCode = nativeCode;
    result.operation  = operation;
    return result;
}

OutputLeaseFailure leaseFailureFor(HostStatus status) noexcept
{
    switch (status)
    {
        case HostStatus::DeviceLost:
            return OutputLeaseFailure::DeviceLost;
        case HostStatus::Timeout:
            return OutputLeaseFailure::Timeout;
        default:
            return OutputLeaseFailure::SubmitFailed;
    }
}

struct OutputPayload
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Fence>    fence;
    std::uint64_t          fenceValue = 0;
    SurfaceExtent          extent;
};

// Validates native publication before entering the CPU offer queue.
struct ValidatedOfferGate : OfferGate
{
    IUnknown*         deviceIdentity = nullptr;
    std::atomic<bool> deviceLostSeen{ false };

    // Publication requires actual native completion; a removal sentinel is
    // never a completed value.
    bool validate(ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value, SurfaceExtent& extent, bool& deviceLost) const
    {
        if (!resource || !fence || value == 0 || value == UINT64_MAX)
        {
            return false;
        }
        const auto desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != kDisplayFormat || desc.MipLevels != 1 ||
            desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 || desc.Width == 0 ||
            desc.Width > kMaxSourceDimension || desc.Height == 0 || desc.Height > kMaxSourceDimension)
        {
            return false;
        }
        ComPtr<ID3D12Device> resourceDevice;
        ComPtr<ID3D12Device> fenceDevice;
        ComPtr<IUnknown>     resourceIdentity;
        ComPtr<IUnknown>     fenceIdentity;
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) ||
            FAILED(resourceDevice.As(&resourceIdentity)) || FAILED(fenceDevice.As(&fenceIdentity)) ||
            resourceIdentity.Get() != deviceIdentity || fenceIdentity.Get() != deviceIdentity)
        {
            return false;
        }
        const auto completed = fence->GetCompletedValue();
        if (FAILED(resourceDevice->GetDeviceRemovedReason()) || completed == UINT64_MAX)
        {
            deviceLost = true;
            return false;
        }
        if (completed < value)
        {
            return false;
        }
        extent.width  = static_cast<std::uint32_t>(desc.Width);
        extent.height = desc.Height;
        return true;
    }

    std::shared_ptr<NativeOutputLease> Offer(ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value, std::optional<std::uint64_t> generation)
    {
        SurfaceExtent   extent;
        bool            deviceLost = false;
        const bool      valid      = validate(resource, fence, value, extent, deviceLost);
        std::lock_guard lock(mutex);
        if (deviceLost)
        {
            deviceLostSeen.store(true, std::memory_order_release);
            accepting = false;
            terminateLocked(OutputLeaseFailure::DeviceLost);
        }
        if (!valid)
        {
            ledger.OnRejected();
            return nullptr;
        }
        auto payload        = std::make_shared<OutputPayload>();
        payload->resource   = resource;
        payload->fence      = fence;
        payload->fenceValue = value;
        payload->extent     = extent;
        return offerLocked(std::move(payload), generation);
    }
};

// Destruction runs in reverse declaration order, so the host and its device
// outlive every object created from them.
struct GpuState
{
    std::unique_ptr<PlumeHost>                   host;
    NativeOverlayCompositor                      overlays;
    std::unique_ptr<plume::RenderShader>         vertexShader;
    std::unique_ptr<plume::RenderShader>         pixelShader;
    std::unique_ptr<plume::RenderSampler>        sampler;
    std::unique_ptr<plume::RenderDescriptorSet>  descriptorSet;
    std::unique_ptr<plume::RenderPipelineLayout> pipelineLayout;
    std::unique_ptr<plume::RenderPipeline>       pipeline;
    ComPtr<ID3D12Resource>                       display;
    SurfaceExtent                                displayExtent;
    D3D12_RESOURCE_STATES                        displayState           = D3D12_RESOURCE_STATE_COPY_DEST;
    bool                                         displayValid           = false;
    bool                                         idlePresented          = false;
    std::uint64_t                                idleOverlaySequence    = 0;
    std::uint64_t                                displayFrameId         = 0;
    std::uint64_t                                displayGammaGeneration = 0;
    std::uint64_t                                copySubmissions        = 0;
    std::uint32_t                                nextPresentSlot        = 0;
    bool                                         captureArmed           = false;
    std::vector<std::uint8_t>                    capturedBackbuffer;
    SurfaceExtent                                capturedExtent;
};

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> createReadbackBuffer(ID3D12Device* device, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = bytes;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
    {
        return nullptr;
    }
    return buffer;
}

void recordTextureToBuffer(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, ID3D12Resource* texture, DXGI_FORMAT format, SurfaceExtent extent, std::uint32_t rowPitch)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource                          = buffer;
    destination.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format   = format;
    destination.PlacedFootprint.Footprint.Width    = extent.width;
    destination.PlacedFootprint.Footprint.Height   = extent.height;
    destination.PlacedFootprint.Footprint.Depth    = 1;
    destination.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource        = texture;
    source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

// Plume's own texture-to-buffer copy dereferences a null destination texture,
// so readbacks and the interop copy use the native list behind the Plume one.
ID3D12GraphicsCommandList* nativeList(plume::RenderCommandList& list) noexcept
{
    auto* d3dList = dynamic_cast<plume::D3D12CommandList*>(&list);
    return d3dList ? d3dList->d3d : nullptr;
}

bool mapRows(ID3D12Resource* buffer, std::uint32_t rowBytes, std::uint32_t rowPitch, std::uint32_t rows, std::uint8_t* destination)
{
    void*       mapped = nullptr;
    D3D12_RANGE read{ 0, static_cast<SIZE_T>(rowPitch) * rows };
    if (FAILED(buffer->Map(0, &read, &mapped)))
    {
        return false;
    }
    for (std::uint32_t row = 0; row < rows; ++row)
    {
        std::memcpy(destination + static_cast<std::size_t>(row) * rowBytes, static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(row) * rowPitch, rowBytes);
    }
    buffer->Unmap(0, nullptr);
    return true;
}

} // namespace

struct NativePresentation::Impl
{
    NativePresentationConfig            config;
    std::shared_ptr<ValidatedOfferGate> gate = std::make_shared<ValidatedOfferGate>();
    // Mirrors gate->surfaceGeneration so a pump result can carry it without the gate lock.
    std::atomic<std::uint64_t>            appliedSurfaceGeneration{ 0 };
    std::mutex                            surfaceMutex;
    SurfaceModel                          surface;
    std::unique_ptr<GpuState>             gpu;
    std::mutex                            captureMutex;
    NativePresentationShutdown            shutdownResult;
    std::shared_ptr<NativeOverlayMailbox> overlays;

    static PresentResult resultFor(const HostResult& host, std::uint64_t frameId = 0)
    {
        PresentResult result;
        result.frameId    = frameId;
        result.nativeCode = host.nativeCode;
        result.operation  = host.operation;
        switch (host.status)
        {
            case HostStatus::Ok:
                result.outcome = PresentOutcome::Presented;
                break;
            case HostStatus::Occluded:
                result.outcome = PresentOutcome::Occluded;
                break;
            case HostStatus::OutOfDate:
                result.outcome = PresentOutcome::OutOfDate;
                break;
            case HostStatus::DeviceLost:
                result.outcome = PresentOutcome::DeviceLost;
                break;
            case HostStatus::Timeout:
                result.outcome = PresentOutcome::Timeout;
                break;
            default:
                result.outcome = PresentOutcome::Failed;
                break;
        }
        return result;
    }

    // Stops admission and ends every pending lease truthfully.
    void terminateAdmission()
    {
        const auto      health = gpu->host->Health();
        std::lock_guard lock(gate->mutex);
        gate->accepting = false;
        gate->terminateLocked(health == HostHealth::DeviceLost ? OutputLeaseFailure::DeviceLost : OutputLeaseFailure::HostClosed);
    }

    PresentResult failure(const HostResult& host, std::uint64_t frameId = 0)
    {
        terminateAdmission();
        return resultFor(host, frameId);
    }

    bool healthy()
    {
        if (gate->deviceLostSeen.load(std::memory_order_acquire))
        {
            gpu->host->NoteDeviceLost();
        }
        gpu->host->PollDeviceLost();
        return gpu->host->Health() == HostHealth::Healthy;
    }

    bool createPipeline(std::string& error)
    {
        auto& device      = gpu->host->Device();
        gpu->vertexShader = device.createShader(kNativePresentVertexDxil, sizeof(kNativePresentVertexDxil), "VSMain", plume::RenderShaderFormat::DXIL);
        gpu->pixelShader  = device.createShader(kNativePresentPixelDxil, sizeof(kNativePresentPixelDxil), "PSMain", plume::RenderShaderFormat::DXIL);

        plume::RenderSamplerDesc samplerDesc;
        samplerDesc.minFilter  = config.nearestScaling ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
        samplerDesc.magFilter  = samplerDesc.minFilter;
        samplerDesc.mipmapMode = plume::RenderMipmapMode::NEAREST;
        samplerDesc.addressU   = plume::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressV   = plume::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressW   = plume::RenderTextureAddressMode::CLAMP;
        samplerDesc.maxLOD     = 0.0F;
        gpu->sampler           = device.createSampler(samplerDesc);

        const plume::RenderDescriptorRange ranges[2] = {
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
        };
        const plume::RenderDescriptorSetDesc setDesc(ranges, 2);
        gpu->descriptorSet = device.createDescriptorSet(setDesc);
        if (!gpu->vertexShader || !gpu->pixelShader || !gpu->sampler || !gpu->descriptorSet)
        {
            error = "Plume could not create the presentation shaders, sampler or descriptor set";
            return false;
        }
        gpu->descriptorSet->setSampler(1, gpu->sampler.get());

        plume::RenderPipelineLayoutDesc layoutDesc;
        layoutDesc.descriptorSetDescs      = &setDesc;
        layoutDesc.descriptorSetDescsCount = 1;
        gpu->pipelineLayout                = device.createPipelineLayout(layoutDesc);
        const auto* d3dLayout              = dynamic_cast<plume::D3D12PipelineLayout*>(gpu->pipelineLayout.get());
        if (!d3dLayout || !d3dLayout->rootSignature)
        {
            error = "Plume could not create the presentation pipeline layout";
            return false;
        }

        plume::RenderGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.pipelineLayout        = gpu->pipelineLayout.get();
        pipelineDesc.vertexShader          = gpu->vertexShader.get();
        pipelineDesc.pixelShader           = gpu->pixelShader.get();
        pipelineDesc.renderTargetFormat[0] = plume::RenderFormat::R8G8B8A8_UNORM;
        pipelineDesc.renderTargetBlend[0]  = plume::RenderBlendDesc::Copy();
        pipelineDesc.renderTargetCount     = 1;
        pipelineDesc.primitiveTopology     = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        gpu->pipeline                      = device.createGraphicsPipeline(pipelineDesc);
        // Plume leaves a null native state when pipeline creation fails.
        const auto* d3dPipeline = dynamic_cast<plume::D3D12GraphicsPipeline*>(gpu->pipeline.get());
        if (!d3dPipeline || !d3dPipeline->d3d)
        {
            error = "Plume could not create the presentation pipeline";
            return false;
        }
        return true;
    }

    // Replaces the display texture when the producer extent changes. The
    // device is idle before the old texture and its descriptor are replaced.
    HostResult ensureDisplay(SurfaceExtent extent, OperationDeadline& deadline)
    {
        HostResult ok;
        ok.status    = HostStatus::Ok;
        ok.operation = "ensure display texture";
        if (gpu->display && gpu->displayExtent == extent)
        {
            return ok;
        }
        auto& host = *gpu->host;
        if (auto drained = host.Drain(deadline); !drained.Ok())
        {
            return drained;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = extent.width;
        desc.Height           = extent.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kDisplayFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> display;
        const HRESULT          created = host.NativeDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&display));
        if (FAILED(created))
        {
            host.NoteFailed();
            host.PollDeviceLost();
            return hostFailed("create display texture", created);
        }
        auto* d3dSet = dynamic_cast<plume::D3D12DescriptorSet*>(gpu->descriptorSet.get());
        if (!d3dSet)
        {
            host.NoteFailed();
            return hostFailed("bind display texture");
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format                  = kDisplayFormat;
        view.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels     = 1;
        d3dSet->setSRV(0, display.Get(), &view);
        gpu->display        = std::move(display);
        gpu->displayExtent  = extent;
        gpu->displayState   = D3D12_RESOURCE_STATE_COPY_DEST;
        gpu->displayValid   = false;
        gpu->displayFrameId = 0;
        return ok;
    }

    // Copy source to display and retire lease after copy fence. Any error fails
    // a claimed lease; after submission also retain its payload. Return failure
    // terminal result, or nothing on success.
    std::optional<PresentResult> copyFrame(const std::shared_ptr<NativeOutputLease>& lease, OperationDeadline& deadline)
    {
        auto&       host    = *gpu->host;
        const auto* payload = static_cast<const OutputPayload*>(lease->Payload().get());
        const auto  failed  = [&](const HostResult& cause)
        {
            lease->Fail(leaseFailureFor(cause.status));
            {
                std::lock_guard lock(gate->mutex);
                gate->ledger.OnCopyFailed();
            }
            return std::optional<PresentResult>(failure(cause, lease->Token()));
        };
        const auto completed = payload->fence->GetCompletedValue();
        if (completed == UINT64_MAX || FAILED(host.NativeDevice()->GetDeviceRemovedReason()))
        {
            host.NoteDeviceLost();
            lease->Fail(OutputLeaseFailure::DeviceLost);
            HostResult lost;
            lost.status    = HostStatus::DeviceLost;
            lost.operation = "producer fence";
            return failure(lost, lease->Token());
        }
        if (completed < payload->fenceValue)
        {
            // The producer published before native completion; nothing was used.
            {
                std::lock_guard lock(gate->mutex);
                gate->ledger.OnStale();
            }
            host.NoteFailed();
            return failed(hostFailed("source not complete"));
        }
        if (auto ensured = ensureDisplay(payload->extent, deadline); !ensured.Ok())
        {
            return failed(ensured);
        }
        if (auto begun = host.BeginSlot(kCopySlot, deadline); !begun.Ok())
        {
            return failed(begun);
        }
        auto* list = nativeList(host.SlotList(kCopySlot));
        if (!list)
        {
            host.NoteFailed();
            return failed(hostFailed("copy command list"));
        }
        transition(list, gpu->display.Get(), gpu->displayState, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(gpu->display.Get(), payload->resource.Get());
        transition(list, gpu->display.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!lease->MarkCopySubmitted(++gpu->copySubmissions))
        {
            host.NoteFailed();
            return failed(hostFailed("mark copy submitted"));
        }
        const auto submitted = host.SubmitSlot(kCopySlot, nullptr, nullptr);
        if (!submitted.Ok())
        {
            // A rejected submission may still have issued earlier steps.
            lease->Fail(leaseFailureFor(submitted.status));
            retainForProcessLifetime(lease->Payload());
            {
                std::lock_guard lock(gate->mutex);
                gate->ledger.OnCopyFailed();
            }
            return failure(submitted, lease->Token());
        }
        gpu->displayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        {
            std::lock_guard lock(gate->mutex);
            gate->ledger.OnCopySubmitted();
        }
        const auto waited = host.WaitSlot(kCopySlot, deadline, DeadlinePhase::Copy);
        if (!waited.Ok())
        {
            lease->Fail(leaseFailureFor(waited.status));
            retainForProcessLifetime(lease->Payload());
            {
                std::lock_guard lock(gate->mutex);
                gate->ledger.OnCopyFailed();
            }
            return failure(waited, lease->Token());
        }
        if (!lease->Retire())
        {
            host.NoteFailed();
            return failed(hostFailed("retire copy"));
        }
        gpu->displayValid           = true;
        gpu->displayFrameId         = lease->Token();
        gpu->displayGammaGeneration = lease->GammaGeneration();
        std::lock_guard lock(gate->mutex);
        gate->ledger.OnCopyRetired(lease->Token());
        return std::nullopt;
    }

    PresentResult presentDisplay(OperationDeadline& deadline, bool newFrame)
    {
        auto&         host   = *gpu->host;
        const auto    extent = SurfaceExtent{ host.SwapChain().getWidth(), host.SwapChain().getHeight() };
        std::uint32_t index  = 0;
        if (auto acquired = host.AcquireBackbuffer(index, deadline); !acquired.Ok())
        {
            return failure(acquired, gpu->displayFrameId);
        }
        const std::size_t slot = kFirstPresentSlot + (gpu->nextPresentSlot++ % kPresentSlotCount);
        if (auto begun = host.BeginSlot(slot, deadline); !begun.Ok())
        {
            return failure(begun, gpu->displayFrameId);
        }
        auto& list       = host.SlotList(slot);
        auto* backbuffer = host.SwapChain().getTexture(index);
        list.barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(backbuffer, plume::RenderTextureLayout::COLOR_WRITE));
        list.setFramebuffer(host.Framebuffer(index));
        list.clearColor(0, plume::RenderColor(0.0F, 0.0F, 0.0F, 1.0F));
        // An idle canvas has no display texture; the clear is the whole image.
        if (gpu->displayValid)
        {
            const auto box = ComputeLetterbox(gpu->displayExtent, extent);
            list.setViewports(plume::RenderViewport(static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.width), static_cast<float>(box.height)));
            list.setScissors(plume::RenderRect(static_cast<std::int32_t>(box.x), static_cast<std::int32_t>(box.y), static_cast<std::int32_t>(box.x + box.width), static_cast<std::int32_t>(box.y + box.height)));
            list.setGraphicsPipelineLayout(gpu->pipelineLayout.get());
            list.setPipeline(gpu->pipeline.get());
            list.setGraphicsDescriptorSet(gpu->descriptorSet.get(), 0);
            list.drawInstanced(3, 1, 0, 0);
        }

        const auto overlay = overlays ? overlays->Latest() : nullptr;
        if ((overlays && overlays->Failed()) || !gpu->overlays.Record(host.Device(), list, slot, overlay, extent.width, extent.height))
        {
            host.NoteFailed();
            if (overlays)
                overlays->Fail();
            return failure({ HostStatus::Failed, 0, "record overlay" }, gpu->displayFrameId);
        }

        ComPtr<ID3D12Resource> capture;
        const auto             capturePitch = alignUp(extent.width * 4U, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        if (gpu->captureArmed)
        {
            gpu->captureArmed = false;
            auto* d3dTexture  = dynamic_cast<plume::D3D12Texture*>(backbuffer);
            auto* d3dList     = nativeList(list);
            capture           = createReadbackBuffer(host.NativeDevice(), static_cast<UINT64>(capturePitch) * extent.height);
            if (capture && d3dTexture && d3dList)
            {
                list.barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(backbuffer, plume::RenderTextureLayout::COPY_SOURCE));
                recordTextureToBuffer(d3dList, capture.Get(), d3dTexture->d3d, DXGI_FORMAT_R8G8B8A8_UNORM, extent, capturePitch);
            }
            else
            {
                capture.Reset();
            }
        }
        list.barriers(plume::RenderBarrierStage::NONE, plume::RenderTextureBarrier(backbuffer, plume::RenderTextureLayout::PRESENT));
        const auto submitted = host.SubmitSlot(slot, host.AcquireSemaphore(), host.ReleaseSemaphore(index));
        if (!submitted.Ok())
        {
            (void)capture.Detach();
            return failure(submitted, gpu->displayFrameId);
        }
        if (gpu->displayValid)
        {
            std::lock_guard lock(gate->mutex);
            gate->ledger.OnHostSubmitted();
        }
        if (capture)
        {
            if (auto waited = host.WaitSlot(slot, deadline, DeadlinePhase::Host); !waited.Ok())
            {
                (void)capture.Detach();
                return failure(waited, gpu->displayFrameId);
            }
            std::lock_guard lock(captureMutex);
            gpu->capturedBackbuffer.resize(static_cast<std::size_t>(extent.width) * extent.height * 4U);
            if (mapRows(capture.Get(), extent.width * 4U, capturePitch, extent.height, gpu->capturedBackbuffer.data()))
            {
                gpu->capturedExtent = extent;
            }
            else
            {
                gpu->capturedBackbuffer.clear();
            }
        }
        const auto presented = host.Present(index);
        bool       repeat    = false;
        if (gpu->displayValid)
        {
            std::lock_guard lock(gate->mutex);
            repeat = gate->ledger.OnPresent(gpu->displayFrameId, presented.status);
        }
        if (presented.status != HostStatus::Ok && presented.status != HostStatus::Occluded && presented.status != HostStatus::OutOfDate)
        {
            return failure(presented, gpu->displayFrameId);
        }
        auto result            = resultFor(presented, gpu->displayFrameId);
        result.gammaGeneration = gpu->displayGammaGeneration;
        if (overlay)
        {
            result.overlaySequence = overlay->sequence;
            for (const auto& batch : overlay->batches)
                result.overlayDraws += static_cast<uint32_t>(batch.commands.size());
        }
        result.newFrame = newFrame;
        result.repeat   = repeat;
        if (!gpu->displayValid)
        {
            gpu->idlePresented       = true;
            gpu->idleOverlaySequence = result.overlaySequence;
        }
        return result;
    }

    PresentResult pump(OperationDeadline& deadline, bool claimNew, bool forceRepaint)
    {
        if (!gpu)
        {
            PresentResult closed;
            closed.operation = "presentation is shut down";
            return closed;
        }
        auto& host = *gpu->host;
        if (!healthy())
        {
            terminateAdmission();
            return resultFor(host.Health() == HostHealth::DeviceLost ? HostResult{ HostStatus::DeviceLost, 0, "device removed" } : hostFailed("host is not healthy"));
        }
        bool                         suspended = false;
        std::optional<SurfaceExtent> pending;
        {
            std::lock_guard lock(surfaceMutex);
            suspended = surface.Suspended();
            pending   = surface.PendingResize();
        }
        const auto suspendedResult = [&](const char* why)
        {
            gate->setSuspended(true);
            {
                std::lock_guard lock(gate->mutex);
                gate->ledger.OnSuspendedPump();
            }
            PresentResult result;
            result.outcome   = PresentOutcome::Suspended;
            result.operation = why;
            return result;
        };
        if (suspended)
        {
            return suspendedResult("suspended");
        }
        if (pending)
        {
            SurfaceExtent actual;
            bool          performed = false;
            const auto    resized   = host.ResizeSwapChain(deadline, actual, performed);
            if (!resized.Ok())
            {
                return failure(resized);
            }
            if (!performed)
            {
                {
                    std::lock_guard lock(surfaceMutex);
                    surface.RequestExtent({});
                }
                return suspendedResult("empty client area");
            }
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(surfaceMutex);
                surface.MarkApplied(actual, *pending);
                generation = surface.Generation();
            }
            std::lock_guard lock(gate->mutex);
            gate->surfaceGeneration = generation;
            appliedSurfaceGeneration.store(generation, std::memory_order_relaxed);
            gate->ledger.OnResize(true);
        }
        bool          newFrame = false;
        std::uint64_t frameId  = 0;
        if (claimNew)
        {
            if (auto lease = gate->claimNewest())
            {
                if (auto failed = copyFrame(lease, deadline))
                {
                    return *failed;
                }
                newFrame = true;
                frameId  = lease->Token();
            }
        }
        bool repaint = false;
        {
            std::lock_guard lock(surfaceMutex);
            repaint = surface.TakeRepaint();
        }
        if (!gpu->displayValid && config.idleCanvas)
        {
            const auto     overlay  = overlays ? overlays->Latest() : nullptr;
            const uint64_t sequence = overlay ? overlay->sequence : 0;
            if (repaint || forceRepaint || !gpu->idlePresented || sequence != gpu->idleOverlaySequence)
            {
                return presentDisplay(deadline, false);
            }
        }
        if ((!newFrame && !repaint && !forceRepaint) || !gpu->displayValid)
        {
            PresentResult none;
            none.outcome   = PresentOutcome::NoFrame;
            none.operation = "no frame";
            none.frameId   = frameId;
            return none;
        }
        return presentDisplay(deadline, newFrame);
    }
};

NativePresentation::NativePresentation()
: impl_(std::make_unique<Impl>())
{
}

NativePresentation::~NativePresentation()
{
    if (impl_ && impl_->gpu)
    {
        auto deadline = OperationDeadline::After(impl_->config.waitCap);
        Shutdown(deadline);
    }
}

std::unique_ptr<NativePresentation> NativePresentation::Create(const NativePresentationConfig& config, std::string& error)
{
    error.clear();
    if (!config.nativeWindow || config.maxQueuedOffers == 0)
    {
        error = "native presentation requires a window and a nonzero offer capacity";
        return nullptr;
    }
    std::unique_ptr<NativePresentation> session(new NativePresentation);
    auto&                               impl = *session->impl_;
    impl.config                              = config;
    impl.gate->capacity                      = config.maxQueuedOffers;
    impl.gpu                                 = std::make_unique<GpuState>();
    PlumeHost::Config hostConfig;
    hostConfig.nativeWindow    = config.nativeWindow;
    hostConfig.bufferCount     = config.bufferCount;
    hostConfig.maxFrameLatency = config.maxFrameLatency;
    hostConfig.workSlots       = kWorkSlotCount;
    hostConfig.vsync           = config.vsync;
    hostConfig.waitCap         = config.waitCap;
    impl.gpu->host             = PlumeHost::Create(hostConfig, error);
    if (!impl.gpu->host)
    {
        impl.gpu.reset();
        return nullptr;
    }
    {
        ComPtr<ID3D12Device> nativeDevice(impl.gpu->host->NativeDevice());
        ComPtr<IUnknown>     identity;
        if (FAILED(nativeDevice.As(&identity)))
        {
            error = "presentation device identity is unavailable";
            impl.gpu.reset();
            return nullptr;
        }
        // The gate compares identity pointers only; the host owns the device.
        impl.gate->deviceIdentity = identity.Get();
    }
    if (!impl.createPipeline(error))
    {
        impl.gpu.reset();
        return nullptr;
    }
    const SurfaceExtent chain{ impl.gpu->host->SwapChain().getWidth(), impl.gpu->host->SwapChain().getHeight() };
    impl.surface.RequestExtent(chain);
    impl.surface.MarkApplied(chain, chain);
    impl.surface.TakeRepaint();
    impl.gate->surfaceGeneration = impl.surface.Generation();
    impl.appliedSurfaceGeneration.store(impl.surface.Generation(), std::memory_order_relaxed);
    impl.gate->ledger.SetPresentInterval(impl.gpu->host->PresentInterval());
    return session;
}

ID3D12Device* NativePresentation::Device() const noexcept
{
    return impl_->gpu ? impl_->gpu->host->NativeDevice() : nullptr;
}

ID3D12CommandQueue* NativePresentation::commandQueueForTesting() const
{
    auto* queue = dynamic_cast<plume::D3D12CommandQueue*>(&impl_->gpu->host->Queue());
    return queue ? queue->d3d : nullptr;
}

NativeTitleOutputPublisher NativePresentation::Publisher(std::optional<std::uint64_t> gammaGeneration) const
{
    return [gate = impl_->gate, gammaGeneration](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        return gate->Offer(resource, fence, value, gammaGeneration);
    };
}

void NativePresentation::SetGammaGeneration(std::uint64_t generation)
{
    std::lock_guard lock(impl_->gate->mutex);
    impl_->gate->gammaGeneration = generation;
}

void NativePresentation::RequestExtent(SurfaceExtent extent)
{
    bool suspended = false;
    {
        std::lock_guard lock(impl_->surfaceMutex);
        impl_->surface.RequestExtent(extent);
        suspended = impl_->surface.Suspended();
    }
    impl_->gate->setSuspended(suspended);
}

void NativePresentation::SetMinimized(bool minimized)
{
    bool suspended = false;
    {
        std::lock_guard lock(impl_->surfaceMutex);
        impl_->surface.SetMinimized(minimized);
        suspended = impl_->surface.Suspended();
    }
    impl_->gate->setSuspended(suspended);
}

PresentResult NativePresentation::PumpOnce(OperationDeadline& deadline)
{
    auto result              = impl_->pump(deadline, true, false);
    result.surfaceGeneration = impl_->appliedSurfaceGeneration.load(std::memory_order_relaxed);
    return result;
}

PresentResult NativePresentation::Repaint(OperationDeadline& deadline)
{
    auto result              = impl_->pump(deadline, false, true);
    result.surfaceGeneration = impl_->appliedSurfaceGeneration.load(std::memory_order_relaxed);
    return result;
}

void NativePresentation::CloseAdmission(OperationDeadline& deadline)
{
    if (impl_->overlays)
        impl_->overlays->SetReady(false);
    impl_->gate->close(deadline);
}

void NativePresentation::SetOverlayMailbox(std::shared_ptr<NativeOverlayMailbox> mailbox)
{
    impl_->overlays = std::move(mailbox);
    if (impl_->overlays)
        impl_->overlays->SetReady(impl_->gpu != nullptr);
}

size_t NativePresentation::OverlayTextureCount() const
{
    return impl_->gpu ? impl_->gpu->overlays.LiveTextures() : 0;
}

size_t NativePresentation::OverlayGeometryAllocations() const
{
    return impl_->gpu ? impl_->gpu->overlays.GeometryAllocations() : 0;
}

size_t NativePresentation::OverlayPendingTextureUploads() const
{
    return impl_->gpu ? impl_->gpu->overlays.PendingTextureUploads() : 0;
}

NativePresentationShutdown NativePresentation::Shutdown(OperationDeadline& deadline)
{
    auto&                      impl = *impl_;
    NativePresentationShutdown out;
    CloseAdmission(deadline);
    if (!impl.gpu)
    {
        return impl.shutdownResult;
    }
    out.drained = impl.gpu->host->Drain(deadline).Ok();
    impl.gpu->host->FinalizeDrain(out.drained);
    impl.terminateAdmission();
    out.health          = impl.gpu->host->Health();
    out.retained        = impl.gpu->host->RetainsOwnership();
    impl.shutdownResult = out;
    if (out.retained)
    {
        REXLOG_ERROR("Native presentation drain is unproved; retaining its GPU owners until process exit");
        (void)impl.gpu.release();
    }
    else
    {
        impl.gpu.reset();
    }
    return out;
}

PresentationCounters NativePresentation::Counters() const
{
    std::lock_guard lock(impl_->gate->mutex);
    return impl_->gate->ledger.Counters();
}

HostHealth NativePresentation::Health() const
{
    return impl_->gpu ? impl_->gpu->host->Health() : impl_->shutdownResult.health;
}

bool NativePresentation::OwnershipRetained() const
{
    return impl_->gpu ? impl_->gpu->host->RetainsOwnership() : impl_->shutdownResult.retained;
}

SurfaceExtent NativePresentation::SwapChainExtent() const
{
    std::lock_guard lock(impl_->surfaceMutex);
    return impl_->surface.Applied();
}

SurfaceExtent NativePresentation::DisplayExtent() const
{
    return impl_->gpu ? impl_->gpu->displayExtent : SurfaceExtent{};
}

bool NativePresentation::CaptureDisplay(std::vector<std::uint32_t>& packed, SurfaceExtent& extent, OperationDeadline& deadline, std::string& error)
{
    error.clear();
    auto& impl = *impl_;
    if (!impl.gpu || !impl.gpu->displayValid)
    {
        error = "no displayed frame to capture";
        return false;
    }
    auto& gpu         = *impl.gpu;
    auto& host        = *gpu.host;
    extent            = gpu.displayExtent;
    const auto pitch  = alignUp(extent.width * 4U, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    auto       buffer = createReadbackBuffer(host.NativeDevice(), static_cast<UINT64>(pitch) * extent.height);
    if (!buffer)
    {
        error = "could not allocate the display readback buffer";
        return false;
    }
    if (auto begun = host.BeginSlot(kCopySlot, deadline); !begun.Ok())
    {
        error = std::string("display capture could not begin: ") + begun.operation;
        return false;
    }
    auto* list = nativeList(host.SlotList(kCopySlot));
    if (!list)
    {
        (void)host.SubmitSlot(kCopySlot, nullptr, nullptr);
        error = "display capture has no native command list";
        return false;
    }
    transition(list, gpu.display.Get(), gpu.displayState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    recordTextureToBuffer(list, buffer.Get(), gpu.display.Get(), kDisplayFormat, extent, pitch);
    transition(list, gpu.display.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu.displayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (auto submitted = host.SubmitSlot(kCopySlot, nullptr, nullptr); !submitted.Ok())
    {
        (void)buffer.Detach();
        error = std::string("display capture submit failed: ") + submitted.operation;
        return false;
    }
    if (auto waited = host.WaitSlot(kCopySlot, deadline, DeadlinePhase::Host); !waited.Ok())
    {
        (void)buffer.Detach();
        error = std::string("display capture wait failed: ") + waited.operation;
        return false;
    }
    packed.resize(static_cast<std::size_t>(extent.width) * extent.height);
    if (!mapRows(buffer.Get(), extent.width * 4U, pitch, extent.height, reinterpret_cast<std::uint8_t*>(packed.data())))
    {
        error = "could not map the display readback buffer";
        return false;
    }
    return true;
}

void NativePresentation::ArmBackbufferCapture()
{
    if (impl_->gpu)
    {
        impl_->gpu->captureArmed = true;
    }
}

bool NativePresentation::TakeBackbufferCapture(std::vector<std::uint8_t>& rgba, SurfaceExtent& extent)
{
    std::lock_guard lock(impl_->captureMutex);
    if (!impl_->gpu || impl_->gpu->capturedBackbuffer.empty())
    {
        return false;
    }
    rgba   = std::move(impl_->gpu->capturedBackbuffer);
    extent = impl_->gpu->capturedExtent;
    impl_->gpu->capturedBackbuffer.clear();
    return true;
}

void NativePresentation::InjectHostFault(HostOperation operation, HostStatus status, std::uint32_t skip)
{
    if (impl_->gpu)
    {
        impl_->gpu->host->InjectFault(operation, status, skip);
    }
}

} // namespace rerevved::gpu
