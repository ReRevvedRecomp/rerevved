#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gpu/present/native_output_lease.h"
#include "native_draw_replay.h"
#include "native_frame_replay.h"
#include "native_render_frame.h"

namespace rex::ui
{

class Window;

}

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;
struct ID3D12CommandQueue;

namespace rex::ui::d3d12
{

class GpuGuestOutputTicket;

}

namespace rerevved::gpu
{

namespace detail
{

struct NativeRenderFrameTestAccess;

}

using NativeGpuOutputPublisher = std::function<std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>(ID3D12Resource*, ID3D12Fence*, std::uint64_t)>;

struct NativeDrawStreamOutput
{
    // The stream owns the copied B10G10R10X2 table through native completion.
    // At least a fresh output path or returned output must be requested.
    std::filesystem::path          outputPath;
    std::array<std::uint32_t, 256> gammaTable{};
    bool                           returnResolvedOutput = false;
};

#if REREVVED_DEVELOPER
// Attachment-pool miss shape: D3D12 description (DXGI format and resource
// flags), miss count, and frame-gap buckets in
// NativeDrawReplayCacheStats::kAttachmentMissGaps.
struct NativeAttachmentMissShape
{
    std::uint32_t                width = 0, height = 0, samples = 0, format = 0, flags = 0;
    bool                         castable = false;
    std::uint64_t                misses   = 0;
    std::array<std::uint64_t, 9> gaps{};
};
#endif

struct NativeDrawReplayCacheStats
{
    std::uint64_t helperShaderCompilations   = 0;
    std::uint64_t rootSignatureCreations     = 0;
    std::uint64_t pipelineCreations          = 0;
    std::uint64_t descriptorHeapCreations    = 0;
    std::uint64_t immutableConstantCreations = 0;
    std::uint64_t outputBundleCreations      = 0;
    std::uint64_t outputBundleReuses         = 0;
    std::uint64_t drawUploadCreations        = 0;
    std::uint64_t drawUploadReuses           = 0;
    std::uint64_t textureCreations           = 0;
    std::uint64_t textureReuses              = 0;
    std::uint64_t textureUploadCreations     = 0;
    std::uint64_t textureUploadReuses        = 0;
    std::uint64_t colorTargetCreations       = 0;
    std::uint64_t colorTargetReuses          = 0;
    std::uint64_t depthTargetCreations       = 0;
    std::uint64_t depthTargetReuses          = 0;
    std::uint64_t frameAttachmentReuses      = 0;
    // Conservative retained attachment domain bound, zero without a domain.
    std::uint64_t frameAttachmentByteBound    = 0;
    std::uint64_t texturePayloadReuses        = 0;
    std::uint64_t texturePayloadBytes         = 0;
    std::uint64_t texturePreparationCreations = 0;
    std::uint64_t textureVersionReuses        = 0;
    // Texture and sampler descriptors a repeated draw binding left unwritten.
    std::uint64_t descriptorReuses = 0;
    // Frame textures that kept the storage of equal bytes under another payload owner.
    std::uint64_t textureContentReuses = 0;
    // Bytes charged against the renderer's 512 MiB owned bound now, and the
    // most charged since the renderer was created.
    std::uint64_t ownedChargeBytes = 0, ownedChargePeakBytes = 0;
    // Frame attachment creations the attachment pool could not serve, and the
    // bytes of the idle attachments it released, over its cap or for storage
    // pressure or shutdown.
    std::uint64_t attachmentPoolMisses = 0, attachmentPoolEvictedBytes = 0;
#if REREVVED_DEVELOPER
    // Attachment miss gaps count frames since return; new marks a shape never
    // taken back.
    static constexpr std::array<const char*, 4>            kAttachmentTakeSites{ "clear_color", "clear_depth", "replay_color", "replay_depth" };
    static constexpr std::array<const char*, 9>            kAttachmentMissGaps{ "0", "1", "2_3", "4_7", "8_15", "16_31", "32_63", "64", "new" };
    std::array<std::uint64_t, kAttachmentTakeSites.size()> attachmentSiteMisses{};
    std::array<NativeAttachmentMissShape, 8>               attachmentMissShapes{};
    std::uint64_t                                          attachmentMissUntracked = 0, attachmentMissFrames = 0;
#endif
    // Fenced upload pool behind texture uploads and frame arenas: acquisitions
    // served by an idle buffer, buffers created, idle buffers released, and
    // the bytes held idle and in all (idle and leased).
    std::uint64_t uploadPoolHits = 0, uploadPoolCreations = 0, uploadPoolEvictions = 0;
    std::uint64_t uploadPoolIdleBytes = 0, uploadPoolLiveBytes = 0;
    // Fenced pools behind frame CPU textures and frame attachments: creations
    // served by an idle resource, idle resources released, and held bytes.
    std::uint64_t texturePoolHits = 0, texturePoolEvictions = 0, texturePoolIdleBytes = 0, texturePoolPendingBytes = 0;
    std::uint64_t attachmentPoolHits = 0, attachmentPoolEvictions = 0, attachmentPoolIdleBytes = 0;
    // Fenced pool behind frame resolve destinations, as above.
    std::uint64_t resolvePoolHits = 0, resolvePoolEvictions = 0, resolvePoolIdleBytes = 0, resolvePoolPendingBytes = 0;
    // Immutable texture versions frame draws sampled from their stored
    // texture, uploaded into one, and evicted, and the bytes stored now.
    std::uint64_t versionTextureHits = 0, versionTextureUploads = 0, versionTextureEvictions = 0, versionTextureBytes = 0;
    // Frame draw sampler tables written to an owner's sampler heap, draws that
    // reused a written table, and command batches that ended because that
    // heap had no room for another table.
    std::uint64_t samplerTables = 0, samplerTableHits = 0, samplerTableFlushes = 0;
};

// Recorded draws and queue submissions, for the checks that a path did or did
// not submit; kept in every build.
struct NativeDrawReplayCounts
{
    std::uint64_t recordedDraws    = 0;
    std::uint64_t queueSubmissions = 0;
    // Submissions that ended a frame command batch at its draw, sampler or
    // command bound rather than at the end of a request.
    std::uint64_t chunkSubmissions = 0;
};

#if REREVVED_DEVELOPER
struct NativeDrawReplayTimingStats
{
    std::uint64_t recordedDraws            = 0;
    std::uint64_t queueSubmissions         = 0;
    std::uint64_t recordingNanoseconds     = 0;
    std::uint64_t fenceWaitNanoseconds     = 0;
    std::uint64_t textureUploadNanoseconds = 0;
    std::uint64_t drawUploadNanoseconds    = 0;
    std::uint64_t pipelineNanoseconds      = 0; // guest pipeline creation
    // Submissions that ended a frame command batch at its draw, sampler or
    // command bound rather than at the end of a request.
    std::uint64_t chunkSubmissions = 0;
    // Depth surface conversions recorded (NativeRenderFrameSurface::depthKey)
    // and their CPU recording time.
    std::uint64_t surfaceConversions = 0, surfaceConversionNanoseconds = 0;
    // Timed phases sum to recordingNanoseconds; texture_retain keeps a snapshot
    // and texture_upload covers its span.
    static constexpr std::array<const char*, 11>       kRecordingPhases{ "setup", "textures", "texture_compare", "texture_upload", "texture_retain", "samplers", "draw_upload", "views", "pipeline", "draw", "tail" };
    std::array<std::uint64_t, kRecordingPhases.size()> recordingPhaseNanoseconds{};
    // Bytes recorded draws wrote to upload memory while timing is on, in
    // kDrawUploadKinds order: the three stage constant buffers with their
    // cleared tails, vertex bytes and index bytes.
    static constexpr std::array<const char*, 3>        kDrawUploadKinds{ "constants", "vertex", "index" };
    std::array<std::uint64_t, kDrawUploadKinds.size()> drawUploadBytes{};
    // Frame CPU texture preparation before recording while timing is on. It
    // starts after the first timed draw and stops when timing is disabled.
    std::uint64_t texturePrepareNanoseconds = 0;
    // Timing labels follow docs/rexglue-runtime.md#native-frames as
    // tex_<outcome>.
    static constexpr std::array<const char*, 7>        kTextureOutcomes{ "repeat", "match", "frame", "new", "changed", "unretained", "uncached" };
    static constexpr std::size_t                       kFirstUploadOutcome = 3;
    std::array<std::uint64_t, kTextureOutcomes.size()> textureOutcomes{}, textureOutcomeBytes{};
    // Draw stream callers that blocked before the stream took their request,
    // and synchronous callers that blocked on its result, with the time.
    std::uint64_t streamQueueWaits = 0, streamQueueWaitNanoseconds = 0;
    std::uint64_t streamFutureWaits = 0, streamFutureWaitNanoseconds = 0;
    // Draw stream thread time outside its idle wait for requests; owner
    // fence waits (fenceWaitNanoseconds) count as busy.
    std::uint64_t streamBusyNanoseconds = 0;
    // Vertex and index bytes, while timing is on, of frame draws whose
    // shared vertex owner an earlier frame draw uploaded: what a geometry
    // cache keyed by that owner would already hold.
    std::array<std::uint64_t, 2> residentGeometryBytes{};
    // Constant-repeat bytes count blocks repeated by the previous draw or
    // within the command chunk in kConstantStages order.
    static constexpr std::array<const char*, 3>       kConstantStages{ "vertex", "pixel", "shared" };
    std::array<std::uint64_t, kConstantStages.size()> constantRepeatPreviousBytes{}, constantRepeatChunkBytes{};
    // Of recorded draws while timing is on: texture slots whose views a draw slot already held,
    // texture SRVs written (two per written slot), and render target views
    // written for draw color targets.
    std::uint64_t textureDescriptorReuses = 0, textureSrvWrites = 0, rtvWrites = 0;
    // Attachment creation calls after an attachment pool miss, failed ones
    // included, and the time in them, at every take site.
    std::uint64_t attachmentCreations = 0, attachmentCreateNanoseconds = 0;
    // native_vertex_residency counters are zero when off. With timing on,
    // successful recorded draws count resident hits/avoided bytes, first-copy
    // misses/bytes and fallback reasons in kResidentFallbackReasons order.
    static constexpr std::array<const char*, 10> kResidentFallbackReasons{ "outside_frame", "owner", "bounds", "generation", "mutated", "cap", "records", "allocation", "scheduling", "disabled_after_init_failure" };
    std::uint64_t                                residentHits = 0, residentBytesAvoided = 0, residentMisses = 0, residentCopyBytes = 0, residentFallbacks = 0;
    std::array<std::uint64_t, 10>                residentFallbackReasons{};
    // Operational events whether or not timing is on: whole-chunk evictions,
    // expired owners, clears that removed keys, copies that did not end in a
    // recorded draw, and initialization refusals.
    std::uint64_t residentEvictions = 0, residentExpiries = 0, residentClears = 0, residentUploadFailures = 0, residentInitFailures = 0;
    // Gauges now: the fixed chunk footprint held, live keys, their payload
    // bytes, and the footprint of evicted chunks awaiting safe reuse.
    std::uint64_t residentBytes = 0, residentEntries = 0, residentPayloadBytes = 0, residentRetiredBytes = 0;
};
#endif

class NativeRenderFrameTextureVersion
{
public:
    NativeRenderFrameTextureVersion(const NativeRenderFrameTextureVersion&)            = delete;
    NativeRenderFrameTextureVersion& operator=(const NativeRenderFrameTextureVersion&) = delete;

private:
    friend class NativeRendererD3D12;
    friend struct detail::NativeRenderFrameTestAccess;
    NativeRenderFrameTextureVersion() = default;
    struct Impl;
    std::shared_ptr<Impl> impl;
};

class NativeRenderFrameTicket
{
public:
    std::shared_future<NativeDrawReplayResult> Recording() const
    {
        return recording;
    }

    std::shared_future<NativeDrawReplayResult> Completion() const
    {
        return completion;
    }

    std::shared_ptr<const NativeRenderFrameTextureVersion> Export(std::uint64_t persistentKey) const;

private:
    friend class NativeRendererD3D12;
    friend struct detail::NativeRenderFrameTestAccess;
    NativeRenderFrameTicket() = default;
    std::shared_future<NativeDrawReplayResult>                                      recording;
    std::shared_future<NativeDrawReplayResult>                                      completion;
    mutable std::mutex                                                              exportMutex;
    std::map<std::uint64_t, std::shared_ptr<const NativeRenderFrameTextureVersion>> exports;
};

// Waits on the fence's completion event until it reaches value or the
// deadline passes, and returns its completed value; UINT64_MAX after device
// removal, which also ends the wait. Polls if no event can be armed.
std::uint64_t WaitStreamFence(ID3D12Fence* fence, std::uint64_t value, std::chrono::steady_clock::time_point deadline);

class NativeRendererD3D12
{
public:
    static constexpr std::size_t MaxPendingGpuOutputs = 2;
    // At queue depth 1 requests wait for drain; depth 2 permits one queued
    // recording and a third waits before image reads.
    static constexpr std::size_t kDrawStreamQueueDepth = 2;
    // The depth of draw streams started after this call, 1 by default.
    static void SetDefaultDrawStreamQueueDepth(std::size_t depth) noexcept;
    // Direct-stream mode charges each request's bytes and limits admission by
    // retained terminal frames. Retired frames are freed on a deleter thread.
    // The renderer default is off; the title can enable it through this setter.
    static void SetDefaultDirectStream(bool enabled) noexcept;
    enum class GpuOutputStatus
    {
        Pending,
        Completed,
        Canceled,
        Failed
    };

    NativeRendererD3D12();
    ~NativeRendererD3D12();

    NativeRendererD3D12(const NativeRendererD3D12&)            = delete;
    NativeRendererD3D12& operator=(const NativeRendererD3D12&) = delete;

    bool Initialize(rex::ui::Window& window);
    // Process startup only: enabling the debug layer after device creation
    // removes existing D3D12 devices. Live callers inherit the host's settings.
    static void                ConfigureReplayDiagnostics();
    NativeDrawReplayCacheStats ReplayCacheStats() const noexcept;
    NativeDrawReplayCounts     ReplayCounts() const noexcept;
#if REREVVED_DEVELOPER
    NativeDrawReplayTimingStats ReplayTimingStats() const noexcept;
#endif
    NativeDrawReplayResult ReplayOffscreen(const NativeDrawReplayRecipe& recipe);
    NativeDrawReplayResult ReplayFrame(NativeFrameReplayRecipe recipe);
    NativeDrawReplayResult ReplayDrawFrames(const NativeDrawFramesRecipe& recipe);
#if REREVVED_DEVELOPER
    // Times pipeline creations and recording phases into ReplayTimingStats; off by default.
    void SetPipelineTiming(bool enabled) noexcept;
#endif
    // Starts a persistent headless 1280x720 stream; lifecycle operations stay owner-serialized.
    bool                   StartDrawStream(std::string& error, ID3D12Device* sharedDevice = nullptr, NativeGpuOutputPublisher publisher = {});
    bool                   AwaitGpuOutput(std::string& error, std::uint64_t* copyFenceValue = nullptr);
    static GpuOutputStatus PollGpuOutput(const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>& ticket, std::uint64_t* copyFenceValue = nullptr);
    static bool            AwaitGpuOutput(const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>& ticket, std::string& error, std::uint64_t* copyFenceValue = nullptr);
    // Starts the same stream for a title-owned host. The publisher runs once per
    // frame after actual native completion and returns the lease whose
    // retirement releases the output slot; it replaces the SDK ticket path.
    bool                   StartTitleDrawStream(std::string& error, ID3D12Device* sharedDevice, NativeTitleOutputPublisher publisher);
    static GpuOutputStatus PollGpuOutput(const std::shared_ptr<NativeOutputLease>& lease, std::uint64_t* copySubmissionId = nullptr);
    static bool            AwaitGpuOutput(const std::shared_ptr<NativeOutputLease>& lease, std::string& error, std::uint64_t* copySubmissionId = nullptr);
    // Takes owned draws, blocks through native completion, and optionally writes two RGBA8 sample planes
    // plus a packed R10G10B10A2 output selected by the caller's copied gamma table;
    // SubmitDrawFrame and Shutdown may run concurrently, with shutdown draining or cancelling the waiter.
    NativeDrawReplayResult SubmitDrawFrame(
        std::vector<NativeDrawReplayRecipe>   draws,
        const std::filesystem::path&          sampleOutputPath = {},
        std::optional<NativeDrawStreamOutput> output           = std::nullopt);
    // Ordered chunks retain uploads through native use, frame resources through
    // terminal completion, and exports through SDK copy retirement.
    NativeDrawReplayResult SubmitRenderFrame(NativeRenderFrame frame);
    // Recording proves submission; Completion proves the actual terminal fence.
    bool QueueRenderFrame(NativeRenderFrame frame, std::shared_ptr<NativeRenderFrameTicket>& ticket, std::string& error);
    void RequestDrawStreamStop();
    // Records and submits one owned first-pass prefix, without a terminal
    // completion claim or output. SubmitRenderFrame must supply its exact
    // completed graph. Shutdown drains a prefix abandoned before terminal swap.
    bool RecordRenderFramePrefix(NativeRenderFrame prefix, std::string& error);
    // Appends new sealed passes, including fresh target layouts.
    // Shared uploads and texture storage recycle after the prior native fence.
    // Output/version publication still requires the exact terminal graph.
    bool AppendRenderFramePasses(NativeRenderFrame passes, std::string& error);
    // CPU-request future settles after recipe lease release; canonical and
    // native/SDK owners keep independent lifetimes. Readiness means recording
    // only; completionFenceValue covers prefix queue work.
    bool QueueRenderFramePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error);
    // Latches the latest non-zero extent and returns acceptance immediately.
    bool Resize(std::uint32_t width, std::uint32_t height);
    void Shutdown();

    bool Initialized() const noexcept;
    // Whether render frames may bind a Color8888 or Color2101010 guest-word
    // resolve destination as Rgba8 or Rgb10a2; false before initialization.
    bool CastableGuestWordTextures() const noexcept;
    // The started stream's queue fence with a reference added for the caller,
    // who releases it; null before a successful start. Take it before Shutdown.
    // It reports UINT64_MAX once the device is removed.
    ID3D12Fence* AcquireStreamFence() const;

private:
    friend struct detail::NativeRenderFrameTestAccess;
    ID3D12CommandQueue* commandQueueForTesting() const noexcept;
    void                beforeRequestForTesting(std::function<void()> callback);
    std::size_t         stagedImportCountForTesting() const;
    std::uint64_t       joinOwnedWalksForTesting() const noexcept;
    std::uint64_t       ownedWalksForTesting() const noexcept;
    static std::size_t  frameCommandDrawsForTesting() noexcept;
    std::uint64_t       checkedResourceStorageForTesting() const;
    std::uint64_t       retainedUploadStorageForTesting() const;
    bool                startDrawStream(std::string& error, ID3D12Device* sharedDevice, NativeGpuOutputPublisher publisher, NativeTitleOutputPublisher titlePublisher);
    bool                submitRenderFramePasses(NativeRenderFrame passes, bool allowAppend, std::string& error);
    bool                queueRenderFramePasses(NativeRenderFrame passes, bool allowAppend, std::shared_future<NativeDrawReplayResult>& recording, std::string& error);
    void                rendererThreadMain(std::uintptr_t nativeWindow,
                                           std::uint32_t  width,
                                           std::uint32_t  height);
    void                headlessReplayThreadMain(NativeDrawReplayRecipe               recipe,
                                                 std::promise<NativeDrawReplayResult> result);
    void                drawStreamThreadMain();
    void                frameReplayThreadMain(NativeFrameReplayRecipe              recipe,
                                              std::promise<NativeDrawReplayResult> result);
    void                handleRendererFailure();
    void                drawFramesThreadMain(NativeDrawFramesRecipe               recipe,
                                             std::promise<NativeDrawReplayResult> result);

    std::shared_ptr<const void> completedTextureForTesting(const std::shared_ptr<const NativeRenderFrameTextureVersion>& version,
                                                           ID3D12Resource*&                                              resource) const;
    // Every pooled upload buffer, idle or leased, each with a reference added
    // for the caller, who releases it.
    std::vector<ID3D12Resource*> uploadPoolBuffersForTesting() const;
    // Retained Texture, or Attachment, storage; and AddRef'd pooled textures
    // and attachments, idle and pending.
    std::uint64_t                retainedTextureStorageForTesting(bool attachments) const;
    std::vector<ID3D12Resource*> pooledTexturesForTesting() const;
    // AddRef'd stored version textures: all, or the one of a live version
    // (null without one); the store's byte cap; and whether frames use it.
    std::vector<ID3D12Resource*> versionTexturesForTesting() const;
    ID3D12Resource*              versionTextureForTesting(const NativeDrawReplayTextureVersion* version) const;
    void                         setVersionTextureCapForTesting(std::uint64_t bytes);
    void                         setVersionTextureStoreForTesting(bool enabled);
    // Overrides whether admission takes typed guest-word views, until restart.
    void setCastableGuestWordsForTesting(bool castable);
    // native_direct_stream: the requests' charge and the persistent bytes.
    std::pair<std::uint64_t, std::uint64_t> directChargesForTesting() const noexcept;
    // The persistent parts: cached CPU textures, pipelines and structures.
    std::array<std::uint64_t, 3> directPersistentForTesting() const noexcept;
    // Residency counters cover live keys, pending references, chunk footprints,
    // transitions, admissions, and seam entries; UINT64_MAX means no seam yet.
    std::uint64_t vertexResidencyEntriesForTesting() const noexcept;
    std::uint64_t vertexResidencyPendingForTesting() const noexcept;
    std::uint64_t vertexResidencyRetiredBytesForTesting() const noexcept;
    std::uint64_t vertexResidencyAllocatedBytesForTesting() const noexcept;
    std::uint64_t vertexResidencySwapForTesting() const noexcept;
    std::uint64_t vertexResidencyBarriersForTesting() const noexcept;
    std::uint64_t vertexResidencyPreBarriersForTesting() const noexcept;
    std::uint64_t vertexResidencyAdmissionsForTesting() const noexcept;
    std::uint64_t vertexResidencySeamEntriesForTesting(int seam) const noexcept;
    // AddRef'd resident chunks, whose release the caller owns.
    std::vector<ID3D12Resource*> vertexResidencyChunksForTesting() const;
    // Residency faults 1-2 fail metadata/chunk setup; 3-5 fail
    // staging/close/signal, 6 scheduling, 7 clears keys, 8 throws on the next
    // draw.
    void setVertexResidencyFaultForTesting(int fault);
    // The Owned domain's retained bytes, read between frame mutations.
    std::uint64_t vertexResidencyOwnedBytesForTesting() const;
#if REREVVED_DEVELOPER
    // The lock ReplayTimingStats reads its snapshot under.
    std::unique_lock<std::mutex> replayTimingLockForTesting();
#endif

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu
