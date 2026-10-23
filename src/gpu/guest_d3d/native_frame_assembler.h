#pragma once

#include "gpu/completion/completion_ledger.h"
#include "gpu/d3d12/native_render_frame.h"
#include "guest_render_capture.h"
#include "native_draw_dispatch.h"
#include "resolve_surface_registry.h"
#include "solid_draw_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// One guest VdSwap as the detached video delegate records it. The fetch is
// the translated frontbuffer fetch the SDK writes into the swap packet.
struct NativeFrameSwap
{
    std::uint64_t sequence = 0; // delegate swap record, strictly increasing
    // NativeDrawDispatcher::NumberedThrough at the swap; every draw numbered
    // through it must have been recorded before the epoch closes.
    std::uint64_t                numberedThrough     = 0;
    std::uint32_t                frontbufferVirtual  = 0;
    std::uint32_t                frontbufferPhysical = 0;
    std::array<std::uint32_t, 6> fetch{};
    std::uint32_t                textureFormat = 0, colorSpace = 0, width = 0, height = 0;
    // Copied by the caller at the swap; the producer applies it once.
    std::array<std::uint32_t, 256> gammaTable{};
    // The caller supplied an assumed table, not the guest's own ramp.
    bool gammaAssumed = false;
};

enum class NativeFrameStatus : std::uint8_t
{
    Complete,    // every submission admitted and nothing assumed; needs the hooked resolve, clear and gamma
    Provisional, // every submission admitted over an assumed clear, resolve or gamma table
    Partial,     // skip mode dropped raster-only draws or presented-target clears; never acceptance output
    Empty,       // no submissions; nothing to present, CPU-effect completion only
    Failed,      // nothing was submitted, and the session stays failed
    Stopped,     // the completion source is closing; nothing was submitted and nothing failed
};

const char* NativeFrameStatusName(NativeFrameStatus status) noexcept;

struct NativeFrameCoverage
{
    std::uint64_t                                     attempted = 0, admitted = 0;
    std::array<std::uint64_t, kNativeDrawReasonCount> skipped{}, rejected{};
};

// Assumptions retained by the presentation path until its corresponding guest
// clear, scanout resolve and gamma table have been captured.
struct NativeFrameAssumptions
{
    bool clearAssumed   = false; // the target starts transparent black
    bool impliedResolve = false; // the swap implies the frontbuffer resolve
    bool gammaAssumed   = false; // the swap's gamma table is not the guest's
};

// An epoch/span covers all admitted/skipped sequences; Empty0..0 and
// nonpresented Partial carry no frame, keeping prior image. Segments are prefix
// work for mid-epoch fences: no resolve/output/presentation.
struct NativeSpanResolve
{
    std::uint64_t    mark = 0, textureId = 0; // the resolve's op mark and destination
    GuestResolveCopy copy;
};

struct NativeCompletionSpan
{
    std::uint64_t                    epoch = 0, firstSequence = 0, lastSequence = 0;
    NativeFrameStatus                status = NativeFrameStatus::Empty;
    NativeFrameCoverage              coverage;
    NativeFrameAssumptions           assumptions;
    std::optional<NativeRenderFrame> frame;
    // Preserve attachments across segments only if next target equals prior
    // last target. Off-screen resolveMark is its own terminal producer, reads
    // no other span attachments and ends chain; next work replays it.
    bool                            segment     = false;
    std::uint64_t                   resolveMark = 0;
    std::optional<GuestResolveCopy> guestResolve;
    // With resolveGraph, segments may hold off-screen passes/resolves read from
    // destination textures after completion. Such resolves do not end the
    // chain; presented passes resume after them.
    std::vector<NativeSpanResolve> resolves; // a segment's, in op-mark order
};

// A closing source returns kNativeCompletionClosing and stops the assembler
// without failure.
inline constexpr std::string_view kNativeCompletionClosing = "closing";

// A completed resolve readback a source handed over unencoded: Encode
// produces the take TakeGuestImage would have, and Bytes is the host memory
// it holds until released. Safe from any thread, once each.
class IHeldGuestImage
{
public:
    virtual ~IHeldGuestImage()                                = default;
    virtual completion::GuestImageTake Encode() const         = 0;
    virtual std::uint64_t              Bytes() const noexcept = 0;
};

class INativeCompletionSource
{
public:
    virtual ~INativeCompletionSource()                                          = default;
    virtual std::uint64_t Submit(NativeCompletionSpan span, std::string& error) = 0;

    // Called before cutting a segment, serialized with Submit by the assembler.
    // False leaves the epoch intact for a later flush or final swap.
    virtual bool ReadyForSegment() const
    {
        return true;
    }

    virtual std::uint64_t Completed() const = 0;

    virtual completion::GuestImageTake TakeGuestImage(std::uint64_t)
    {
        return {};
    }

    // TakeGuestImage without the encode: a readback still to be encoded
    // comes back in `held` with an empty take; an image already encoded,
    // or a failure, comes back as TakeGuestImage returns it.
    virtual completion::GuestImageTake HoldGuestImage(std::uint64_t opMark, std::shared_ptr<const IHeldGuestImage>& held)
    {
        held.reset();
        return TakeGuestImage(opMark);
    }

    // Encodes the image of the submitted resolve at opMark, waiting up to
    // `timeout` for its readback, without taking it or any older image.
    virtual completion::GuestImageTake PeekGuestImage(std::uint64_t, std::chrono::milliseconds)
    {
        return {};
    }

    // No image at or below opMark will be taken; see IDrawCompletion.
    virtual void DiscardGuestImagesThrough(std::uint64_t opMark)
    {
        static_cast<void>(opMark);
    }

    virtual bool WaitFor(std::uint64_t value, std::chrono::milliseconds timeout) = 0;
};

// Host completion without a renderer. Spans without a frame complete at
// submission; a span with draws to render is refused, so no admitted draw is
// ever reported complete.
class CpuCompletionSource final : public INativeCompletionSource
{
public:
    std::uint64_t Submit(NativeCompletionSpan span, std::string& error) override;
    std::uint64_t Completed() const override;
    bool          WaitFor(std::uint64_t value, std::chrono::milliseconds timeout) override;

private:
    std::atomic<std::uint64_t> completed{ 0 };
};

struct NativeAssembledFrame
{
    std::uint64_t          epoch = 0, swapSequence = 0, firstSequence = 0, lastSequence = 0;
    NativeFrameStatus      status = NativeFrameStatus::Failed;
    NativeFrameCoverage    coverage;
    NativeFrameAssumptions assumptions;
    // Failure cause, or the first skipped submission of a partial frame.
    std::string detail;
    // Retained only when no completion source took the frame.
    std::optional<NativeRenderFrame> frame;
    std::uint64_t                    hostFence = 0;
};

// Builds one submission's native draw from inputs read before Original. False
// with an empty detail means it lies outside every admitted family.
using NativeFrameDrawBuilder = std::function<bool(const NativeDrawSubmission&, NativeDrawReplayRecipe&, std::string& detail)>;

// Coverage requires every sample to be written without pixel kill; alphaOne
// proves SRC_ALPHA/INV_SRC_ALPHA replaces the target.
struct NativeFrameDrawCover
{
    NativeDrawReplayScissor rect;
    bool                    alphaOne = false;

    NativeFrameDrawCover(NativeDrawReplayScissor covered = {}, bool one = false)
    : rect(covered)
    , alphaOne(one)
    {
    }
};

// A family's proof for its pinned programs. Empty proves none.
using NativeFrameDrawCoverage = std::function<std::optional<NativeFrameDrawCover>(const NativeDrawSubmission&, const NativeDrawReplayRecipe&)>;

// Decoded positions are reported raw because recipes carry executable shaders,
// not a CPU position transform. Omitted pixel constant vectors are all zero.
std::string DescribeNativeDrawVisibility(const NativeDrawSubmission& submission, const NativeDrawReplayRecipe& draw);

// Catalog word digests of a draw's bound VS (variant zero) and PS microcode.
// The VS is digested as the draw's fetch patch leaves it (HostPatchedVertexDigest).
struct NativeDrawShaderWords
{
    std::string vertex, pixel;
};

// Reads a submission's shader words before Original. False names the
// unreadable field in `error`.
using NativeDrawShaderReader = std::function<bool(const NativeDrawSubmission&, NativeDrawShaderWords&, std::string& error)>;

// The largest small-read size class: plain reads below it count by size.
inline constexpr std::size_t kNativeMirroredReadBytes = std::size_t(1) << 20;
// The default mirrored read size. A watched hit costs about 0.1 us at any
// size and a 64 KiB copy about 3.5 us; below it the copies saved do not pay
// for the extra mirror entries and watched pages.
inline constexpr std::size_t kNativeMirroredReadMinBytes = std::size_t(64) << 10;

struct NativeFrameAssemblerConfig
{
    // Census drops refused draws and failed segment work without failing later
    // epochs; skip mode drops only NoNativeRoute.
    NativeDrawMode         mode = NativeDrawMode::Strict;
    NativeFrameDrawBuilder build; // empty admits nothing
    // Lets an off-screen draw define an uncleared target: within its covered
    // rectangle and scissor, an opaque color-only draw overwrites every
    // sample, so a resolve inside that rectangle is modeled. Empty defines none.
    NativeFrameDrawCoverage coverage;
    // Not owned and must outlive the assembler; null returns frames to the caller.
    INativeCompletionSource* completion = nullptr;
    // Offline checks read the output back instead of publishing it.
    bool readback = false;
    // native_resolve_graph: off-screen resolves join the epoch's segments as
    // passes with readbacks, continuing a source's attachments between its
    // resolves, instead of each replaying its source in a frame of its own.
    bool resolveGraph = false;
    // With resolveGraph: each graph resolve submits its segment when recorded.
    bool eagerGraphSegments = false;
    // native_resolve_alias, with resolveGraph: off-screen resolves enter the
    // alias table, which ResolvedTexture consults for a draw's fetches.
    bool resolveAlias = false;
    // native_surface_registry holds off-screen resolves for later-frame
    // imports. ResolvedTexture uses it instead of alias table; frames transfer
    // resolve textures in and ended store keys out.
    std::shared_ptr<ResolveSurfaceRegistry> surfaces;
    // The renderer views guest-word resolve textures as Rgba8 or Rgb10a2
    // (NativeRendererD3D12::CastableGuestWordTextures); without it a packed
    // destination is not aliased.
    bool castableGuestWords = false;
    // native_gamma_apply: a swap without presented work whose only change from
    // the shown image is its gamma table adopts the table for the next present
    // instead of failing swap_display_form_changed. A changed swizzle still fails.
    bool adoptGammaWithoutWork = false;
    // native_frame_timing: count resolve graph decisions for ResolveGraphStats
    // and mirrored reads for ReadMirror.
    bool timing = false;
    // Live mirrored read buffers, including those a family still holds, stay
    // within this; a read that does not fit is a plain copy.
    std::size_t mirroredReadBudget = std::size_t(256) << 20;
    // Reads of at least this many bytes are mirrored; smaller ones are plain
    // copies. Every mirror entry holds a counted buffer this large or larger,
    // so the budget also bounds the entries to mirroredReadBudget divided by it.
    std::size_t mirroredReadMinBytes = kNativeMirroredReadMinBytes;
    // GuestWrote, HostWrote, and GuestImageWritten must report every writer
    // before mirrored reads stop comparing.
    std::function<bool(std::uint32_t physical, std::uint32_t length)> watchMirror;
    // Serialized observation of only the first four recorded admitted draws.
    std::function<void(unsigned, const NativeDrawSubmission&, const NativeDrawReplayRecipe&)> observeDraw;
    // Presented decode may be unset/unreadable. First decode sets EDRAM base;
    // other base refuses, same-base shape may differ. Scanout checks
    // base/format, not extent. Off-screen needs matching decode.
    GuestWordReader readTarget;
    // viewTarget returns a guest virtual span when readable; cached
    // target/tiling decode is reused only while its words stay unchanged.
    using TargetView = std::function<std::span<const std::uint8_t>(std::uint32_t address, std::uint32_t size)>;
    TargetView viewTarget;
    // Names the shaders of a draw into an ended tiled MRT set. Only the pinned
    // loading-screen pairs are discarded there; empty discards nothing.
    NativeDrawShaderReader readShaders;
    // Under assembler lock, arm one-shot guest write at scanout/resolveAlias
    // record. GuestWrote never takes that lock. Without notice, unseen writes
    // stay unmodeled and no resolve aliases.
    std::function<void(std::uint32_t physical, std::uint32_t length)> watchWrites;
    // Host writes outside guest stores run under assembler locks and may take
    // only locks not held around assembler calls.
    std::function<void(std::uint64_t begin, std::uint64_t end, bool resolve)> resourceWritten;
};

// Counter names of the eight NativeResolvedTextureMiss alias refusals, in
// order: alias_miss_<name>.
inline constexpr std::array<const char*, 8> kNativeAliasMissNames{ "form", "coverage", "written", "superseded", "retired", "packing", "reset", "offscreen" };

// Counter names of the fetch conditions a form miss failed, in the order
// NativeResolveAliasTable checks them; the first failing one counts:
// alias_form_<name>.
inline constexpr std::array<const char*, 14> kNativeAliasFormNames{ "format", "type", "base", "endian", "pitch", "size", "stacked", "dimension", "signed", "exp_bias", "mip", "border", "packed_mips", "swizzle" };
static_assert(kNativeAliasFormNames.size() == kResolveSurfaceFormConditions, "the surface view rule counts its refusals under the alias form names");

// An off-screen resolve remains live until later resolve, guest write, or epoch
// end; admitted graph segments can sample it on GPU.
struct NativePendingResolve
{
    // textureId stays zero until a graph segment admits the resolve. chain is
    // the open chain holding its texture, zero once that chain was reset.
    std::uint64_t mark = 0, textureId = 0, chain = 0, epoch = 0;
    // The copy's destination fetch and info words, as the encoder checked them.
    std::array<std::uint32_t, 6> destFetch{};
    std::uint32_t                destInfo = 0;
    std::uint32_t                destBase = 0;
    std::uint64_t                destEnd  = 0;
    // The rectangle the resolve wrote, at its destination origin.
    std::uint32_t          destX = 0, destY = 0, width = 0, height = 0;
    NativeGuestWordPacking packing = NativeGuestWordPacking::None;
    // Guest writes numbered above this, reported over the range, invalidate
    // it: the watch is armed at record, before the image can land.
    std::uint64_t watchedFrom = 0;
    // The ledger wrote the image; a draw bound the texture.
    bool landed = false, bound = false;
};

// The binding an aliased texture slot takes, and the resolve that produced it.
struct NativeResolvedAlias
{
    NativeResolvedTexture texture;
    std::uint64_t         mark = 0;
};

using NativeAliasLookup = std::variant<NativeResolvedAlias, NativeResolvedTextureMiss>;

// Pending graph resolves by destination range, in op-mark order. Neutral:
// only the predicate lives here; its owner serializes access.
class NativeResolveAliasTable
{
public:
    // Older entries beyond this are forgotten, so their readers take the bytes path.
    static constexpr std::size_t kEntries = 256;

    // A later resolve over an earlier one's range supersedes it. A resolve
    // without a textureId is not admitted yet. A bound entry is kept for its
    // epoch's re-records, so with every entry bound the resolve is not aliased.
    void Register(const NativePendingResolve& resolve)
    {
        Supersede(resolve.destBase, resolve.destEnd);
        if (entries.size() == kEntries)
        {
            const auto unbound = std::find_if(entries.begin(), entries.end(), [](const Entry& entry)
                                              {
                                                  return !entry.resolve.bound;
                                              });
            if (unbound == entries.end())
                return;
            entries.erase(unbound);
        }
        entries.push_back({ resolve, std::nullopt });
    }

    // Another resolve, recorded or not, wrote [begin, end).
    void Supersede(std::uint64_t begin, std::uint64_t end)
    {
        for (auto& entry : entries)
            if (!entry.ended && overlaps(entry.resolve, begin, end))
                entry.ended = NativeResolvedTextureMiss::Superseded;
    }

    // A guest write reported as `number`, or without one any other writer,
    // over [begin, end). A write numbered at or below an entry's watchedFrom
    // predates its arming and was already in guest memory.
    void Written(std::uint64_t begin, std::uint64_t end, std::optional<std::uint64_t> number = std::nullopt)
    {
        for (auto& entry : entries)
            if (!entry.ended && overlaps(entry.resolve, begin, end) && (!number || *number > entry.resolve.watchedFrom))
                entry.ended = NativeResolvedTextureMiss::Written;
    }

    // A graph segment of `chain` recorded the resolve at `mark` into `textureId`.
    void Admit(std::uint64_t mark, std::uint64_t textureId, std::uint64_t chain)
    {
        if (auto* entry = live(mark))
        {
            entry->resolve.textureId = textureId;
            entry->resolve.chain     = chain;
        }
    }

    // The ledger wrote the image at `mark`.
    void Landed(std::uint64_t mark)
    {
        if (auto* entry = live(mark))
            entry->resolve.landed = true;
    }

    // The ledger never writes the images marked [first, last] not yet landed,
    // so guest memory keeps bytes other than their textures.
    void Unwritten(std::uint64_t first, std::uint64_t last)
    {
        for (auto& entry : entries)
            if (!entry.ended && !entry.resolve.landed && first <= entry.resolve.mark && entry.resolve.mark <= last)
                entry.ended = NativeResolvedTextureMiss::Written;
    }

    // A draw bound the texture of the resolve at `mark`.
    void Bind(std::uint64_t mark)
    {
        if (auto* entry = live(mark))
            entry->resolve.bound = true;
    }

    // The chain was reset: its textures leave the frames that follow, so its
    // entries wait for the next chain or chainless frame to re-record them.
    void EndChain(std::uint64_t chain)
    {
        for (auto& entry : entries)
            if (entry.resolve.chain == chain)
                entry.resolve.chain = 0;
    }

    // The bound entries of `epoch` no chain holds, by mark, now held by
    // `chain`: a new chain or a chainless frame re-records each, ended or not,
    // since draws bound it while it was live.
    std::vector<std::uint64_t> Adopt(std::uint64_t epoch, std::uint64_t chain)
    {
        std::vector<std::uint64_t> marks;
        for (auto& entry : entries)
            if (entry.resolve.epoch == epoch && entry.resolve.bound && !entry.resolve.chain)
            {
                entry.resolve.chain = chain;
                marks.push_back(entry.resolve.mark);
            }
        return marks;
    }

    // The epoch ended or was abandoned: no later frame re-records its textures.
    void EndEpoch(std::uint64_t epoch)
    {
        for (auto& entry : entries)
            if (entry.resolve.epoch == epoch)
            {
                entry.ended         = NativeResolvedTextureMiss::Retired;
                entry.resolve.bound = false;
            }
    }

    // Latest resolve before opMark(zero:after all recorded) covering fetch
    // base; only open epoch and holding chain alias. No covering range=nullopt;
    // formRefusal names first failing condition.
    std::optional<NativeAliasLookup> Find(const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark, std::uint64_t epoch, std::uint64_t chain, bool castable, std::size_t* formRefusal = nullptr) const
    {
        const std::uint64_t base = fetch[1] & kPhysicalPages;
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
        {
            const auto& resolve = entry->resolve;
            if ((opMark && resolve.mark >= opMark) || base < resolve.destBase || base >= resolve.destEnd)
                continue;
            if (entry->ended)
                return *entry->ended;
            if (!resolve.textureId)
                return NativeResolvedTextureMiss::Unmodeled;
            if (resolve.epoch != epoch)
                return NativeResolvedTextureMiss::Retired;
            if (resolve.chain != chain)
                return NativeResolvedTextureMiss::Reset;
            NativeResolvedAlias alias;
            if (const auto refusal = form(fetch, resolve, alias); refusal < kNativeAliasFormNames.size())
            {
                if (formRefusal)
                    *formRefusal = refusal;
                return NativeResolvedTextureMiss::Form;
            }
            if (resolve.destX || resolve.destY || resolve.width != alias.texture.layout.width || resolve.height != alias.texture.layout.height)
                return NativeResolvedTextureMiss::Coverage;
            if (resolve.packing != NativeGuestWordPacking::None && !castable)
                return NativeResolvedTextureMiss::Packing;
            alias.texture.frameTextureId = resolve.textureId;
            alias.mark                   = resolve.mark;
            return alias;
        }
        return std::nullopt;
    }

    std::size_t Size() const noexcept
    {
        return entries.size();
    }

private:
    struct Entry
    {
        NativePendingResolve                     resolve;
        std::optional<NativeResolvedTextureMiss> ended;
    };

    std::vector<Entry> entries;

    Entry* live(std::uint64_t mark)
    {
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
            if (entry->resolve.mark == mark)
                return entry->ended ? nullptr : &*entry;
        return nullptr;
    }

    // A fetch's base page as the bytes path reads it: the guest may name a
    // virtual view, whose upper bits GetSubresourcesFromFetchConstant drops.
    static constexpr std::uint32_t kPhysicalPages = 0x1FFFF000U;

    static bool overlaps(const NativePendingResolve& resolve, std::uint64_t begin, std::uint64_t end)
    {
        return begin < resolve.destEnd && resolve.destBase < end;
    }

    // Resolve fetch match requires format 6/54 view, base, size, pitch, tiling,
    // endian, one 2D level, unsigned and unbiased decode.
    static std::size_t form(const std::array<std::uint32_t, 6>& fetch, const NativePendingResolve& resolve, NativeResolvedAlias& alias)
    {
        const auto& dest   = resolve.destFetch;
        const auto  format = fetch[1] & 0x3F;
        const bool  rgba8  = format == 6 && (resolve.packing == NativeGuestWordPacking::None || resolve.packing == NativeGuestWordPacking::Color8888);
        const bool  wide   = format == 54 && resolve.packing == NativeGuestWordPacking::Color2101010;
        // In kNativeAliasFormNames order; the swizzle is checked last, below.
        const std::array<bool, kNativeAliasFormNames.size() - 1> refused{
            !rgba8 && !wide,
            (fetch[0] & 3) != 2,
            (fetch[1] & kPhysicalPages) != resolve.destBase,
            ((fetch[1] >> 6) & 3) != ((dest[1] >> 6) & 3),
            (fetch[0] >> 22) != (dest[0] >> 22),
            (fetch[2] & 0x3FFFFFFU) != (dest[2] & 0x3FFFFFFU),
            (fetch[2] >> 26) || (fetch[1] & 0x400),
            ((fetch[5] >> 9) & 3) != 1,
            (fetch[0] & 0x3FC) || (fetch[3] & 1),
            ((fetch[3] >> 13) & 63) != 0,
            ((fetch[4] >> 2) & 0xFF) != 0,
            (fetch[5] & 4) != 0,
            (fetch[5] & 0x800) != (dest[5] & 0x800),
        };
        if (const auto first = std::find(refused.begin(), refused.end(), true); first != refused.end())
            return static_cast<std::size_t>(first - refused.begin());
        alias.texture.layout = { (fetch[2] & 8191) + 1, ((fetch[2] >> 13) & 8191) + 1, rgba8 ? NativeDrawReplayTextureFormat::Rgba8 : NativeDrawReplayTextureFormat::Rgb10a2, 1, NativeDrawReplayTextureDimension::Texture2D };
        // The guest bytes hold red and blue exchanged when the copy swapped
        // them; the host texture does not, so the selectors exchange instead.
        const bool                            swapped = resolve.destInfo & (1U << 24);
        constexpr std::array<std::uint8_t, 4> kSwapped{ 2, 1, 0, 3 };
        for (std::size_t i = 0; i < alias.texture.swizzle.size(); ++i)
        {
            const auto selector = static_cast<std::uint8_t>((fetch[3] >> (1 + 3 * i)) & 7);
            if (selector > 5)
                return kNativeAliasFormNames.size() - 1;
            alias.texture.swizzle[i] = selector < 4 && swapped ? kSwapped[selector] : selector;
        }
        return kNativeAliasFormNames.size();
    }
};

// Each Swap yields one result; Failed submits no span and ends the session even
// in skip mode.
struct NativeResolveGraphStats
{
    std::uint64_t continuations = 0; // passes recording only new draws
    std::uint64_t replays       = 0; // passes replaying the whole source
    std::uint64_t notReady      = 0; // the source could not cut a segment
    std::uint64_t signed16      = 0; // a k_16_16 target slot
    std::uint64_t noSource      = 0; // no recorded source content
    std::uint64_t swapBatch     = 0; // another open resolve at the swap fell back
    // ResolvedTexture refusals by kNativeAliasMissNames; a fetch no resolve
    // covers, or one not admitted to the graph, counts in none.
    std::array<std::uint64_t, kNativeAliasMissNames.size()> aliasMisses{};
    // The form misses among them by their first failing kNativeAliasFormNames condition.
    std::array<std::uint64_t, kNativeAliasFormNames.size()> aliasForms{};
    std::uint64_t                                           aliasHits       = 0;     // fetches bound to a resolve texture
    std::uint64_t                                           aliasReproduced = 0;     // resolves re-recorded after a chain reset
    bool                                                    castable        = false; // config.castableGuestWords
};

class GuestReadTemplates;
struct GuestReadSnapshot;

class NativeFrameAssembler final : public completion::IDrawCompletion, public IGuestRenderSink
{
public:
    explicit NativeFrameAssembler(NativeFrameAssemblerConfig config);
    ~NativeFrameAssembler() override;

    NativeFrameAssembler(const NativeFrameAssembler&)            = delete;
    NativeFrameAssembler& operator=(const NativeFrameAssembler&) = delete;

    // Routes the dispatcher's mode, admission and observers here. Its hooks
    // capture this, so the assembler outlives that dispatcher and any hook
    // still in flight, including after a timed-out RemoveNativeDrawDispatcher.
    void          Attach(NativeDrawDispatchConfig& dispatch);
    std::uint64_t NumberOperation();

    // The dispatcher seam; offline tests drive it directly.
    void Began(const NativeDrawSubmission& submission) noexcept;
    // Builds a direct lowerer root that Began held, at its commit.
    void                            Committed(const NativeDrawSubmission& submission) noexcept;
    std::optional<NativeDrawReason> Admit(const NativeDrawSubmission& submission) const noexcept;
    // A rejected draw that began here copies its family refusal detail to
    // `refusal`, empty when the families gave none.
    std::uint64_t Recorded(const NativeDrawSubmission& submission, std::optional<NativeDrawReason> verdict, std::string* refusal = nullptr) noexcept;
    std::uint64_t Clear(const GuestClear& clear, std::string& error) override;
    std::uint64_t Resolve(const GuestTarget& target, const GuestResolveCopy& copy, bool frontbuffer, bool& partial, std::string& error) override;
    void          ClearUnreadable(const std::optional<GuestTarget>& decoded) override;
    void          ResolveUnmodeled(const std::optional<GuestResolveCopy>& copy) override;
    // The scanout shape outside a tiled texture pass: once a pass's depth
    // tile opened it, its color tiles from the presented surface are
    // textures, as pass 3's are. The tiled scanout pair has no depth tile.
    bool ScanoutCandidate(const GuestTarget& target, const GuestResolveCopy& copy) const override;
    // Hands the open epoch's drawing thread to the caller.
    void DeviceAcquired() noexcept override;
    // A guest write to physical memory that a watchWrites notice reported.
    // Safe from any thread, including a memory access handler. It also ends
    // the watch of any mirrored read the range overlaps.
    void GuestWrote(std::uint32_t physical, std::uint32_t length) noexcept;
    // End the watch of mirrored reads the ledger's host write overlaps; while
    // one is between them, no mirrored read is watched again, so none is
    // trusted over bytes the write may not have reached.
    void HostWriting(std::uint32_t physical, std::uint32_t length) noexcept override;
    void HostWrote(std::uint32_t physical, std::uint32_t length) noexcept override;
    // The SDK's physical memory invalidation callback, with the assembler as
    // its context: reports the write and permits unwatching only the pages it
    // names, so another watched range keeps its notice.
    static std::pair<std::uint32_t, std::uint32_t> WriteNotice(void* context, std::uint32_t physical, std::uint32_t length, bool exact) noexcept;

    // ReadResolved overlays pending images in op order after
    // submission/readback, without assembler locks. Retry/readback waits are
    // bounded; blocking source submission or swap-held submission lock is not.
    struct ReadResolvedTrace
    {
        bool          overlapped = false, flushed = false;
        std::uint64_t mark = 0, begin = 0, end = 0;
        std::uint64_t lockNanoseconds = 0;
    };

    bool ReadResolved(std::uint32_t                                     address,
                      std::uint32_t                                     size,
                      const std::function<std::vector<std::uint8_t>()>& read,
                      std::vector<std::uint8_t>&                        bytes,
                      std::string&                                      error,
                      ReadResolvedTrace*                                trace = nullptr);
    // view reads validated guest memory only when no unwritten image overlays
    // the range; otherwise it refreshes through ReadResolved.
    bool ReadResolved(std::uint32_t                                         address,
                      std::uint32_t                                         size,
                      const std::function<std::span<const std::uint8_t>()>& view,
                      GuestBytes&                                           bytes,
                      std::string&                                          error,
                      ReadResolvedTrace*                                    trace = nullptr);
    // flushed marks an unwritten image lacking image or held readback; the
    // query reads no bytes and serves diagnostics.
    ReadResolvedTrace ProbeResolved(std::uint32_t address, std::uint32_t size) const;
    // A view is empty over an unwritten resolve image; callers validate earlier
    // bytes by reading again.
    std::span<const std::uint8_t> ViewResolved(std::uint32_t address, std::uint32_t size, const std::function<std::span<const std::uint8_t>()>& view) const;
    // The largest small-read size class.
    static constexpr std::size_t kMirroredReadBytes = kNativeMirroredReadBytes;

    // Without config.surfaces, the family query takes only the alias leaf lock
    // and never submits, waits, or peeks.
    std::optional<NativeResolvedTexture> ResolvedTexture(const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark, NativeResolvedTextureMiss& miss) noexcept;

    struct ReadMirrorStats
    {
        // With config.timing, mirrored reads whose bytes equalled the
        // previous read of the same range, and those that differed or had none.
        std::uint64_t hits = 0, misses = 0, hitBytes = 0, missBytes = 0;
        // The hits a watched range served without a compare.
        std::uint64_t watchedHits = 0, watchedHitBytes = 0;
        // Bytes of live mirror buffers, always counted.
        std::uint64_t retainedBytes = 0;
        // Timing buckets copied unmirrored reads below 4 KiB, 64 KiB, 256 KiB,
        // and 1 MiB.
        static constexpr std::array<std::size_t, 4> kSmallReadLimits{ std::size_t(4) << 10, std::size_t(64) << 10, std::size_t(256) << 10, kMirroredReadBytes };
        std::array<std::uint64_t, 4>                smallReads{}, smallBytes{};
    };

    ReadMirrorStats ReadMirror() const;

    NativeAssembledFrame       Swap(const NativeFrameSwap& swap) noexcept;
    std::optional<std::string> Failure() const;
    // True once the completion source refused work because it is closing.
    bool Stopped() const;

    // Sequence fence submission uses one off-screen resolve frame or one
    // segment; unready sources retry and failed submission fails the session.
    completion::DrawFlush FlushThrough(std::uint64_t sequence) override;
    // The completion source's value; zero without one.
    std::uint64_t Completed() const override;
    // With surfaces and image writes, live unlocked entries stay
    // unencoded(take.held) until lock, owed ending or read. Older held images
    // under a new write come first(take.before).
    completion::GuestImageTake TakeGuestImage(std::uint64_t opMark) override;
    // Held images stay: their records were applied.
    void          DiscardGuestImagesThrough(std::uint64_t opMark) override;
    void          GuestImageWritten(std::uint64_t opMark) override;
    void          GuestImageSkipped(std::uint64_t opMark) override;
    std::uint64_t LastSubmitted() const override;
    // Lock-free; zero unless the config sets timing.
    NativeResolveGraphStats ResolveGraphStats() const noexcept;
    // The ledger's image-write gate, given once the ledger exists. Without
    // it no image is held. Call before draws start.
    void SetGuestImageWrites(completion::GuestImageWrites writes);
    // The ranges its draws' builds read, which a deferred draw captures.
    GuestReadTemplates& ReadTemplates() const noexcept;
    // Deferred builds that read outside their snapshot see mutable memory and
    // are refused before admission.
    void BeganSequenced(const NativeDrawSubmission& submission, const GuestReadSnapshot* snapshot) noexcept;
    // Held resolve images beyond these are written when taken.
    static constexpr std::size_t   kHeldImages = 128;
    static constexpr std::uint64_t kHeldBytes  = std::uint64_t(128) << 20;

private:
    struct State;
    NativeAssembledFrame assemble(const NativeFrameSwap& swap);
    // Builds this thread's draft and checks the draw against its target.
    void build(const NativeDrawSubmission& submission) noexcept;
    // Sets resolveOwed when it submitted a resolve and another through
    // `sequence` remains.
    std::uint64_t flush(std::uint64_t sequence, std::string& error, bool& retry, bool& resolveOwed);
    // Under swapMutex. Records the span's fence; `work` is its first operation
    // with host work, or zero. A closing source sets Stopped and returns zero.
    std::uint64_t submit(NativeCompletionSpan span, std::uint64_t work, std::string& error);
    // Census mode, under swapMutex: drops a failed epoch's segment chain and
    // ends the segments the source still holds with a terminal frame.
    void abandonEpoch(std::uint64_t epoch);
    // TakeGuestImage with the registry and the ledger's gate.
    completion::GuestImageTake takeHeld(std::uint64_t mark);
    std::unique_ptr<State>     state;
};

// The M1a panel boundary: 0x826A3568 from its two callers, stride 8.
bool ReadSolidPanelInputs(const NativeDrawSubmission& submission, const ReadMemory& read, SolidDrawInputs& inputs, std::string& error);
bool BuildSolidPanelDraw(const SolidDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail);

NativeFrameDrawBuilder MakeSolidPanelDrawBuilder(ReadMemory read, std::shared_ptr<const NativeGameplayShaderInterface> shader);
// Rejected-draw diagnostics:hook/caller, UP stride(indexed r1+84, nonindexed
// r7), shader objects, PS word digest, texture fetch 0(G+0x480). Name
// unreadable fields, never guess.
std::string DescribeNativeDrawSubmission(const NativeDrawSubmission& submission, const ReadMemory& read);
// Patched VS digest reads required G+0x3198/G+0x3194 objects;
// native_draw_shader_words.cpp owns the patch model.
bool ReadNativeDrawShaderWords(const NativeDrawSubmission& submission, const ReadMemory& read, NativeDrawShaderWords& words, std::string& error);
// Use raw view for virtual ranges or while assembler is null; otherwise
// physical ranges use ViewResolved. Exceptions from raw return an empty span.
GuestMemoryView MakeResolvedGuestView(std::function<const NativeFrameAssembler*()> assembler, GuestMemoryView raw);

// Virtual guest ranges a draw build read, (address, size), in read order.
using GuestReadSet = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

// CPU-side build bytes are captured; GPU-lifetime bytes are read physically and
// not held.
struct GuestReadSnapshot
{
    struct Range
    {
        std::uint32_t             address = 0;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Range> ranges; // by address, disjoint

    // The held bytes of [address, address + size), or an empty span when no
    // one range holds all of them.
    std::span<const std::uint8_t> Find(std::uint64_t address, std::uint64_t size) const noexcept;
    std::uint64_t                 Bytes() const noexcept;
};

// While one is open on this thread, SnapshotReads serves virtual reads from
// its snapshot and notes each in its read set; a read the snapshot does not
// hold is late and reads guest memory. Scopes nest; the innermost applies.
class GuestReadSnapshotScope
{
public:
    GuestReadSnapshotScope(const GuestReadSnapshot* snapshot, GuestReadSet* reads) noexcept;
    ~GuestReadSnapshotScope();
    GuestReadSnapshotScope(const GuestReadSnapshotScope&)            = delete;
    GuestReadSnapshotScope& operator=(const GuestReadSnapshotScope&) = delete;

    // A virtual read under the innermost scope: the snapshot's bytes, or
    // nullopt for guest memory (late when the scope has a snapshot).
    static std::optional<std::span<const std::uint8_t>> View(std::uint64_t address, std::uint64_t size) noexcept;

    std::uint64_t Late() const noexcept
    {
        return late;
    }

private:
    const GuestReadSnapshot* snapshot;
    GuestReadSet*            reads;
    std::uint64_t            late = 0;
    GuestReadSnapshotScope*  outer;
};

// `read` with its virtual reads and views served through the scope above;
// with no scope open it reads as `read` does.
ReadMemory SnapshotReads(ReadMemory read);

// Identity(hook/caller/shader/declaration) learns merged ranges for next
// Capture plus dynamic reads. Excluded/overbound/UP draws never capture; UP
// builds before guest reuses caller memory.
class GuestReadTemplates
{
public:
    GuestReadTemplates();
    ~GuestReadTemplates();

    // Adds what one build of `submission` read, `late` of them outside its
    // snapshot; reads inside `dynamic` are left to DynamicReads.
    void Learn(const NativeDrawSubmission& submission, const GuestReadSet& reads, std::uint64_t late = 0, const GuestReadSet& dynamic = {});
    // Ends the capture of the identity's draws.
    void Exclude(const NativeDrawSubmission& submission);
    // The ranges a draw's own state names, which differ between draws of
    // one identity: an indexed buffer draw's index buffer header (the
    // object at G+0x3094). `read` reads one guest word.
    static GuestReadSet DynamicReads(const NativeDrawSubmission& submission, const std::function<bool(std::uint32_t, std::uint32_t&)>& read);
    // Reads deferred builds made outside their snapshots, in all.
    std::uint64_t LateReads() const noexcept;
    // False when the identity has no template, outgrew it, or a range has
    // no view; `snapshot` is then unspecified.
    bool Capture(const NativeDrawSubmission& submission, const GuestMemoryView& view, GuestReadSnapshot& snapshot) const;

    static constexpr std::size_t   kMaxRanges  = 64;
    static constexpr std::uint64_t kMaxBytes   = 64 * 1024;
    static constexpr std::size_t   kMaxEntries = 8192;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu::guest_d3d
