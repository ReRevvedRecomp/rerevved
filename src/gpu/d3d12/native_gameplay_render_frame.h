#pragma once

#include "gpu/native_gameplay_snapshot.h"
#include "native_draw_replay.h"
#include "native_gameplay_draw.h"
#include "native_render_frame.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rerevved::gpu
{

class NativeGameplayResolveAdmission;
class NativeGameplayCommandFeed;

// Frame resources are native resolve destinations. Canonical guest bytes stay
// on recipe texture bindings; making a captured Xenos copy a resource would
// depend on pixel readback instead of its native producer.
struct NativeGameplayRenderFrameResource
{
    NativeRenderFrameTexture texture;
    bool                     generated = true;
};

struct NativeGameplayRenderFrameDraw
{
    std::uint64_t                                    eventId = 0;
    NativeDrawReplayRecipe                           recipe;
    bool                                             alphaTestEnabled = true;
    std::shared_ptr<const NativeDrawReplayAdmission> admittedRecipe;

    const NativeDrawReplayRecipe& Recipe() const noexcept
    {
        return admittedRecipe ? admittedRecipe->Recipe() : recipe;
    }
};

// Canonical guest layout captured from the resolve producer. A consumer fetch
// must match this record before an unavailable native source may be bound.
struct NativeGameplayGuestTextureLayout
{
    bool          valid            = false;
    std::uint32_t basePage         = 0;
    std::uint32_t mipPage          = 0;
    std::uint32_t width            = 0;
    std::uint32_t height           = 0;
    std::uint32_t depthOrArraySize = 1;
    std::uint32_t pitch            = 0;
    std::uint32_t format           = 0;
    std::uint32_t endianness       = 0;
    std::uint32_t mipMinLevel      = 0;
    std::uint32_t mipMaxLevel      = 0;
    bool          tiled            = false;
    bool          packedMips       = false;

    friend bool operator==(const NativeGameplayGuestTextureLayout&, const NativeGameplayGuestTextureLayout&) = default;
};

struct NativeGameplayRenderFrameCopy
{
    enum class Semantics : std::uint8_t
    {
        // The captured 640x720 scene uses the Xenos tiled half-copy layout.
        TiledMainColor,
        TiledMainDepth,
        // A full-height resolve whose source and destination rectangles were
        // already derived from the observer's SDK copy state.
        Explicit,
    };

    std::uint64_t                eventId                   = 0;
    std::uint64_t                triggerEventId            = 0;
    bool                         sourceGeometryOwnedByCopy = false;
    Semantics                    semantics                 = Semantics::Explicit;
    std::vector<std::uint32_t>   registers;
    std::uint32_t                sourceX              = 0;
    std::uint32_t                sourceY              = 0;
    std::uint32_t                sourceWidth          = 0;
    std::uint32_t                sourceHeight         = 0;
    std::uint32_t                destinationX         = 0;
    std::uint64_t                destinationTextureId = 0;
    bool                         depth                = false;
    std::uint32_t                colorTarget          = 0;
    std::array<std::uint32_t, 2> samples              = { 0, 0 };
    bool                         swapRedBlue          = false;
    float                        valueScale           = 1.0F;
    // Overrides the input-wide anchor for captures with more than one tiled
    // color/depth image. Zero selects the input-wide value.
    std::uint64_t                                         tiledSceneBase = 0;
    NativeGameplayGuestTextureLayout                      producerLayout;
    std::shared_ptr<const NativeGameplayResolveAdmission> admittedOperation;
};

class NativeGameplayResolveAdmission
{
public:
    const NativeRenderFrameResolve& Resolve() const noexcept
    {
        return resolve_;
    }

    const NativeRenderFramePass::Clear& Clear() const noexcept
    {
        return clear_;
    }

    bool HasClear() const noexcept
    {
        return clear_.color || clear_.depth;
    }

    std::size_t MetadataBytes() const noexcept
    {
        return sizeof(*this);
    }

    bool Validate(const NativeGameplayRenderFrameCopy& copy, std::string& error) const;

private:
    friend class NativeGameplayCommandFeed;
    NativeGameplayResolveAdmission(const NativeGameplayRenderFrameCopy& copy,
                                   NativeRenderFrameResolve             resolve,
                                   NativeRenderFramePass::Clear         clear,
                                   std::array<std::uint8_t, 24>         rectangle,
                                   std::uint64_t                        geometryEvent,
                                   std::uint32_t                        geometryAddress);
    NativeGameplayResolveAdmission(const NativeGameplayResolveAdmission&)                                = delete;
    NativeGameplayResolveAdmission&                     operator=(const NativeGameplayResolveAdmission&) = delete;
    const NativeGameplayRenderFrameCopy                 copy_;
    const NativeRenderFrameResolve                      resolve_;
    const NativeRenderFramePass::Clear                  clear_;
    const std::array<std::uint8_t, 24>                  rectangle_;
    const std::uint64_t                                 geometryEvent_;
    const std::uint32_t                                 geometryAddress_;
    std::weak_ptr<const NativeGameplayResolveAdmission> owner_;
};

struct NativeGameplayRenderFrameOperation
{
    enum class Kind : std::uint8_t
    {
        Draw,
        Copy,
    };

    Kind          kind    = Kind::Draw;
    std::uint64_t eventId = 0;
    std::uint32_t index   = 0;
};

struct NativeGameplayRenderFramePass
{
    std::uint64_t firstEvent = 0;
    std::uint64_t lastEvent  = 0;

    // Aliased EDRAM signatures may differ by pitch/samples. Accept a change
    // only after explicit clear, proved overwrite of every used pixel or native
    // color restoration; never invent an independent attachment.
    std::uint64_t attachmentAlias             = 0;
    bool          attachmentAliasValid        = false;
    bool          clearAliasTransition        = false;
    bool          preservePreviousAttachments = false;
    // A post-copy clear is sourced from the copy event itself and therefore
    // shares its event identity with the preceding resolve pass.
    bool postCopyClear = false;
    // A clear-only pass has no draw recipe. Its canonical target and clear
    // values come from BuildNativeGameplayClearPass; a following copy may
    // remain in the same native pass so the clear executes first.
    NativeRenderFramePass::Target             target;
    std::vector<NativeRenderFramePass::Clear> clears;

    std::vector<NativeGameplayRenderFrameDraw>      draws;
    std::vector<NativeGameplayRenderFrameCopy>      copies;
    std::vector<NativeGameplayRenderFrameOperation> operations;
};

struct NativeGameplayRenderFrameInput
{
    std::vector<NativeGameplayRenderFrameResource> resources;
    std::vector<NativeGameplayRenderFramePass>     passes;
    std::uint64_t                                  outputTextureId = 0;
    std::array<std::uint8_t, 4>                    outputSwizzle   = { 0, 1, 2, 3 };
    std::array<std::uint32_t, 256>                 gammaTable{};
    std::filesystem::path                          outputPath;
    bool                                           returnResolvedOutput = false;
    bool                                           retainOnly           = false;

    // The two accepted main-scene copies use base and base + 0x14000 for the
    // left and right 640-pixel strips. This is required for tiled semantics;
    // Explicit copies do not use it.
    std::uint64_t tiledSceneBase = 0;
};

// Convert one observer copy record to the backend resolve representation. The
// helper is public so the live adapter can use the same strict copy checks as
// offline assembly.
bool DeriveNativeGameplayResolve(const NativeGameplayRenderFrameCopy& copy,
                                 std::uint64_t                        tiledSceneBase,
                                 NativeRenderFrameResolve&            resolve,
                                 std::string&                         error);

// Assemble ordered native draw/copy records into the GPU frame graph. The
// input owns canonical CPU textures through its draw recipes and identifies
// generated resources only by resolve destination ID.
bool BuildNativeGameplayRenderFrame(const NativeGameplayRenderFrameInput& input,
                                    NativeRenderFrame&                    frame,
                                    std::string&                          error);

// Transfer owned draw buffers after checking event order and attachment aliases.
bool BuildNativeGameplayRenderFrame(NativeGameplayRenderFrameInput&& input,
                                    NativeRenderFrame&               frame,
                                    std::string&                     error);

// Absent CPU textures decode only complete static snapshot bytes; copied Xenos
// pages never become native output.
struct NativeGameplayRetainedTexture
{
    std::uint32_t                    guestBasePage = 0;
    NativeRenderFrameTexture         texture;
    NativeGameplayGuestTextureLayout guestLayout;
};

// Prepares each owned event once at the admitted observer boundary. Only the
// terminal swap finalizes attachment lookahead and the output graph. Proposed
// native versions must not be published until renderer completion.
class NativeGameplayCommandFeed
{
public:
    using PrefixConsumer      = std::function<bool(NativeRenderFrame, std::string&)>;
    using PrefixQueueConsumer = std::function<bool(NativeRenderFrame, std::shared_future<NativeDrawReplayResult>&, std::string&)>;
    enum class PrefixRecording
    {
        // FirstPass is normal. Diagnostic CompatibleChain stops on changed
        // target; TargetChanges continues over fresh unsigned targets.
        FirstPass,
        CompatibleChain,
        TargetChanges,
    };
    // Consumer takes new contiguous sealed unsigned passes; retires handles
    // before return. Signed/copied-input bounds defer to Finish full-graph
    // validation before output. Canonical/GPU owners stay independent.
    void SetPrefixConsumer(PrefixConsumer consumer, PrefixRecording mode = PrefixRecording::FirstPass);
    // Admission transfers the request's operation handles. Its ticket retires
    // that lease; canonical operations and GPU owners retire independently.
    void SetPrefixQueueConsumer(PrefixQueueConsumer consumer, PrefixRecording mode = PrefixRecording::FirstPass);
    // The immutable catalog and optional geometry owner outlive the feed.
    explicit NativeGameplayCommandFeed(std::span<const NativeGameplayShaderInterface> shaders,
                                       NativeGameplayGeometryCache*                   geometryCache = nullptr);
    NativeGameplayCommandFeed(std::span<const NativeGameplayShaderInterface> shaders,
                              NativeGameplayGeometryCache*                   geometryCache,
                              NativeGameplayPreparationContext&              preparation);
    ~NativeGameplayCommandFeed();
    NativeGameplayCommandFeed(const NativeGameplayCommandFeed&)            = delete;
    NativeGameplayCommandFeed& operator=(const NativeGameplayCommandFeed&) = delete;
    static std::size_t         CopyStateBytes() noexcept;
    // Inspect frame-owned operations until the next frame mutation.
    const NativeGameplayRenderFrameCopy* Copy(std::uint64_t eventId) const noexcept;
    const NativeGameplayRenderFrameDraw* Draw(std::uint64_t eventId) const noexcept;

    bool Begin(std::uint64_t startFrame, std::uint64_t startSubmission, std::span<const NativeGameplayRetainedTexture> retained, std::string& error, std::span<const std::uint32_t> retainPages = {}, bool retainAll = true, bool trackRetained = true);
    bool Begin(std::uint64_t startFrame, std::uint64_t startSubmission, std::span<const NativeGameplayRetainedTexture> retained, std::string& error, NativeGameplayPreparationContext& preparation);
    bool Consume(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                 std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                 const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                 std::string&                                                   error);
    // The live bank is borrowed for this call. Prepared recipes own every
    // value needed after it returns; the diagnostic event may omit its bank.
    bool Consume(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                 std::span<const std::uint32_t>                                 registers,
                 std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                 const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                 std::string&                                                   error);
    // Admits CPU-owned draws with unfinished original execution provenance.
    bool                             ConsumePreparedBoundary(const rex::graphics::diagnostic::DrawCaptureFrameEvent& event,
                                                             std::span<const std::uint32_t>                          registers,
                                                             std::string&                                            error);
    bool                             ConsumePreparedDraw(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                                                         std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                                                         const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                                                         std::string&                                                   error);
    bool                             ConsumePreparedDraw(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                                                         std::span<const std::uint32_t>                                 registers,
                                                         std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                                                         const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                                                         std::string&                                                   error);
    bool                             ConsumePreparedCopy(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                                                         std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                                                         const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                                                         std::string&                                                   error);
    bool                             ConsumePreparedCopy(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                                                         std::span<const std::uint32_t>                                 registers,
                                                         std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                                                         const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                                                         std::string&                                                   error);
    bool                             Finish(const rex::graphics::diagnostic::DrawCaptureFrameEvent& terminal,
                                            const rex::graphics::diagnostic::DrawCaptureFinalSwap&  swap,
                                            std::uint64_t                                           endFrame,
                                            std::uint64_t                                           endSubmission,
                                            NativeRenderFrame&                                      frame,
                                            std::vector<NativeGameplayRetainedTexture>&             nextRetained,
                                            std::string&                                            error);
    bool                             FinishProducer(std::uint32_t page, std::uint64_t producerEvent, NativeRenderFrame& frame, std::string& error);
    bool                             FinishPrepared(const rex::graphics::diagnostic::DrawCaptureFrameEvent& terminal,
                                                    const rex::graphics::diagnostic::DrawCaptureFinalSwap&  swap,
                                                    std::uint64_t                                           endFrame,
                                                    std::uint64_t                                           endSubmission,
                                                    NativeRenderFrame&                                      frame,
                                                    std::vector<NativeGameplayRetainedTexture>&             nextRetained,
                                                    std::string&                                            error);
    NativeGameplayPreparationReceipt PreparationReceipt(const NativeRenderFrame& frame, const std::vector<NativeGameplayRetainedTexture>& nextRetained);

private:
    bool ConsumeEvent(const rex::graphics::diagnostic::DrawCaptureFrameEvent&        event,
                      std::span<const std::uint32_t>                                 registers,
                      std::span<const rex::graphics::diagnostic::DrawCaptureTexture> textures,
                      const std::map<std::uint32_t, NativeDrawReplayTexture>&        cpuTextures,
                      std::string&                                                   error,
                      bool                                                           prepared,
                      bool                                                           preparedCopy     = false,
                      bool                                                           preparedBoundary = false);
    bool FinishFrame(const rex::graphics::diagnostic::DrawCaptureFrameEvent& terminal,
                     const rex::graphics::diagnostic::DrawCaptureFinalSwap&  swap,
                     std::uint64_t                                           endFrame,
                     std::uint64_t                                           endSubmission,
                     NativeRenderFrame&                                      frame,
                     std::vector<NativeGameplayRetainedTexture>&             nextRetained,
                     std::string&                                            error,
                     bool                                                    prepared);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// For CPU-owned recordings, retain the latest resolve to each requested page.
// A page may first be produced during this frame. Publish nextRetained only
// after successful submission on the same renderer; it contains no CPU pixels.
bool BuildNativeGameplaySequenceFrame(
    const NativeGameplaySnapshot&                  snapshot,
    std::span<const NativeGameplayShaderInterface> shaders,
    std::span<const NativeGameplayRetainedTexture> retained,
    std::span<const std::uint32_t>                 retainPages,
    NativeRenderFrame&                             frame,
    std::vector<NativeGameplayRetainedTexture>&    nextRetained,
    std::string&                                   error);

// Carry latest native versions within frame bounds; CPU uses retire pages.
// Publish nextRetained after GPU completion. Geometry uses spare planning
// budget; every reuse validates state/exact bytes.
bool BuildNativeGameplayLiveFrame(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    std::span<const NativeGameplayRetainedTexture>          retained,
    NativeRenderFrame&                                      frame,
    std::vector<NativeGameplayRetainedTexture>&             nextRetained,
    std::string&                                            error,
    NativeGameplayGeometryCache*                            geometryCache   = nullptr,
    NativeGameplayCommandFeed::PrefixConsumer               prefixConsumer  = {},
    NativeGameplayCommandFeed::PrefixRecording              prefixRecording = NativeGameplayCommandFeed::PrefixRecording::FirstPass,
    NativeGameplayCommandFeed::PrefixQueueConsumer          queueConsumer   = {});

bool BuildNativeGameplayLiveFrame(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    std::span<const NativeGameplayRetainedTexture>          retained,
    NativeRenderFrame&                                      frame,
    std::vector<NativeGameplayRetainedTexture>&             nextRetained,
    std::string&                                            error,
    NativeGameplayPreparationContext&                       preparation,
    NativeGameplayPreparationReceipt&                       receipt,
    NativeGameplayGeometryCache*                            geometryCache   = nullptr,
    NativeGameplayCommandFeed::PrefixConsumer               prefixConsumer  = {},
    NativeGameplayCommandFeed::PrefixRecording              prefixRecording = NativeGameplayCommandFeed::PrefixRecording::FirstPass,
    NativeGameplayCommandFeed::PrefixQueueConsumer          queueConsumer   = {});

bool BuildNativeGameplayRenderFrame(
    const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
    std::span<const NativeGameplayShaderInterface>             shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>&    cpuTextures,
    NativeRenderFrame&                                         frame,
    std::string&                                               error,
    const NativeGameplayRetainedTexture*                       retained     = nullptr,
    NativeGameplayRetainedTexture*                             nextRetained = nullptr);

// Convert through the last resolve to the requested page. Its dependencies
// must be available in this snapshot; retained output contains no CPU pixels.
bool BuildNativeGameplayProducer(
    const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
    std::span<const NativeGameplayShaderInterface>             shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>&    cpuTextures,
    std::uint32_t                                              page,
    NativeRenderFrame&                                         frame,
    NativeGameplayRetainedTexture&                             retained,
    std::string&                                               error);

bool BuildNativeGameplayRenderFrame(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    NativeRenderFrame&                                      frame,
    std::string&                                            error,
    const NativeGameplayRetainedTexture*                    retained     = nullptr,
    NativeGameplayRetainedTexture*                          nextRetained = nullptr);

bool BuildNativeGameplayProducer(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    std::uint32_t                                           page,
    NativeRenderFrame&                                      frame,
    NativeGameplayRetainedTexture&                          retained,
    std::string&                                            error);

} // namespace rerevved::gpu
