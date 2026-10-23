#pragma once

// The single definition of the friend access types the renderer and the SDK
// presenter grant to tests. Every test source that needs them includes this
// header; defining either type locally breaks the one-definition rule once all
// tests link into rerevved_tests. This is the union of the accessors the
// native_frame_stream, native_prepared_bc1 and native_frame_assembler_d3d12
// sources defined for themselves. The ticket helpers compile everywhere; the
// renderer and presenter accessors need the Windows D3D12 headers and the
// Windows-only renderer, so they exist only under _WIN32.

#include "gpu/d3d12/native_renderer_d3d12.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <d3d12.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#endif

namespace rerevved::gpu::detail
{

struct NativeRenderFrameTestAccess
{
    // A ticket the renderer never touches; exports appear only when a test
    // publishes them, as the renderer does once a recording completes.
    static std::shared_ptr<NativeRenderFrameTicket> Ticket(std::shared_future<NativeDrawReplayResult> recording,
                                                           std::shared_future<NativeDrawReplayResult> completion)
    {
        auto result        = std::shared_ptr<NativeRenderFrameTicket>(new NativeRenderFrameTicket());
        result->recording  = std::move(recording);
        result->completion = std::move(completion);
        return result;
    }

    static void PublishExports(NativeRenderFrameTicket& ticket, std::span<const std::uint64_t> keys)
    {
        std::lock_guard lock(ticket.exportMutex);
        for (const auto key : keys)
            ticket.exports.emplace(key, std::shared_ptr<const NativeRenderFrameTextureVersion>(new NativeRenderFrameTextureVersion()));
    }

#ifdef _WIN32
    static ID3D12CommandQueue* Queue(const NativeRendererD3D12& renderer)
    {
        return renderer.commandQueueForTesting();
    }

    static void BeforeRequest(NativeRendererD3D12& renderer, std::function<void()> callback)
    {
        renderer.beforeRequestForTesting(std::move(callback));
    }

    static std::uint64_t CheckedResourceStorage(const NativeRendererD3D12& renderer)
    {
        return renderer.checkedResourceStorageForTesting();
    }

    static std::shared_ptr<const void> CompletedTexture(const NativeRendererD3D12&                                    renderer,
                                                        const std::shared_ptr<const NativeRenderFrameTextureVersion>& version,
                                                        ID3D12Resource*&                                              resource)
    {
        return renderer.completedTextureForTesting(version, resource);
    }

    static std::size_t StagedImports(const NativeRendererD3D12& renderer)
    {
        return renderer.stagedImportCountForTesting();
    }

    static std::uint64_t JoinOwnedWalks(const NativeRendererD3D12& renderer)
    {
        return renderer.joinOwnedWalksForTesting();
    }

    static std::uint64_t OwnedWalks(const NativeRendererD3D12& renderer)
    {
        return renderer.ownedWalksForTesting();
    }

    static std::pair<std::uint64_t, std::uint64_t> DirectCharges(const NativeRendererD3D12& renderer)
    {
        return renderer.directChargesForTesting();
    }

    static std::array<std::uint64_t, 3> DirectPersistent(const NativeRendererD3D12& renderer)
    {
        return renderer.directPersistentForTesting();
    }

    static std::size_t FrameCommandDraws()
    {
        return NativeRendererD3D12::frameCommandDrawsForTesting();
    }

    static std::uint64_t RetainedUploadStorage(const NativeRendererD3D12& renderer)
    {
        return renderer.retainedUploadStorageForTesting();
    }

    static std::vector<ID3D12Resource*> UploadPoolBuffers(const NativeRendererD3D12& renderer)
    {
        return renderer.uploadPoolBuffersForTesting();
    }

    static std::uint64_t RetainedTextureStorage(const NativeRendererD3D12& renderer, bool attachments)
    {
        return renderer.retainedTextureStorageForTesting(attachments);
    }

    static std::vector<ID3D12Resource*> PooledTextures(const NativeRendererD3D12& renderer)
    {
        return renderer.pooledTexturesForTesting();
    }

    static std::vector<ID3D12Resource*> VersionTextures(const NativeRendererD3D12& renderer)
    {
        return renderer.versionTexturesForTesting();
    }

    static ID3D12Resource* VersionTexture(const NativeRendererD3D12& renderer, const NativeDrawReplayTextureVersion* version)
    {
        return renderer.versionTextureForTesting(version);
    }

    static void SetVersionTextureCap(NativeRendererD3D12& renderer, std::uint64_t bytes)
    {
        renderer.setVersionTextureCapForTesting(bytes);
    }

    static void SetVersionTextureStore(NativeRendererD3D12& renderer, bool enabled)
    {
        renderer.setVersionTextureStoreForTesting(enabled);
    }

    static void SetCastableGuestWords(NativeRendererD3D12& renderer, bool castable)
    {
        renderer.setCastableGuestWordsForTesting(castable);
    }

    static void ResidencyFault(NativeRendererD3D12& renderer, int fault)
    {
        renderer.setVertexResidencyFaultForTesting(fault);
    }

    static std::uint64_t VertexResidencyEntries(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyEntriesForTesting();
    }

    static std::uint64_t VertexResidencyPending(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyPendingForTesting();
    }

    static std::uint64_t VertexResidencyRetiredBytes(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyRetiredBytesForTesting();
    }

    static std::uint64_t VertexResidencyAllocatedBytes(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyAllocatedBytesForTesting();
    }

    static std::uint64_t VertexResidencyBarriers(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyBarriersForTesting();
    }

    static std::uint64_t VertexResidencyPreBarriers(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyPreBarriersForTesting();
    }

    static std::uint64_t VertexResidencyAdmissions(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyAdmissionsForTesting();
    }

    static std::uint64_t VertexResidencySwap(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencySwapForTesting();
    }

#if REREVVED_DEVELOPER
    static std::unique_lock<std::mutex> ReplayTimingLock(NativeRendererD3D12& renderer)
    {
        return renderer.replayTimingLockForTesting();
    }
#endif

    static std::uint64_t VertexResidencyOwnedBytes(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyOwnedBytesForTesting();
    }

    static std::uint64_t VertexResidencySeamEntries(const NativeRendererD3D12& renderer, int seam)
    {
        return renderer.vertexResidencySeamEntriesForTesting(seam);
    }

    static std::vector<ID3D12Resource*> VertexResidencyChunks(const NativeRendererD3D12& renderer)
    {
        return renderer.vertexResidencyChunksForTesting();
    }
#endif // _WIN32
};

} // namespace rerevved::gpu::detail

#ifdef _WIN32
namespace rex::ui::d3d12::detail
{

// Block the actual SDK consumer queue without blocking native production.
struct GpuGuestOutputTestAccess
{
    static ID3D12CommandQueue* CopyQueue(D3D12Presenter& presenter)
    {
        return presenter.gpu_guest_output_copy_queue_.Get();
    }
};

} // namespace rex::ui::d3d12::detail

#endif // _WIN32
