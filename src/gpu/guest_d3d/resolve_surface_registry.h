#pragma once

#include "gpu/d3d12/native_render_frame.h"
#include "gpu/guest_d3d/guest_resolve_encoder.h"
#include "gpu/guest_d3d/guest_resource_observers.h"
#include "gpu/guest_d3d/solid_draw_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Resolve-mark texture IDs lie above fixed IDs and below graph IDs(1 << 62).
// Renderer store keys lie above legacy resolve keys((destBase << 32)|mark).
// These are separate namespaces.
inline constexpr std::uint64_t kResolveSurfaceTextureBase = std::uint64_t(3) << 60;
inline constexpr std::uint64_t kResolveSurfaceTextureEnd  = std::uint64_t(1) << 62;
inline constexpr std::uint64_t kResolveSurfaceKeyTag      = std::uint64_t(1) << 63;

constexpr std::uint64_t ResolveSurfaceTexture(std::uint64_t mark) noexcept
{
    return kResolveSurfaceTextureBase + mark;
}

constexpr std::uint64_t ResolveSurfaceKey(std::uint64_t mark) noexcept
{
    return kResolveSurfaceKeyTag | mark;
}

constexpr bool IsResolveSurfaceTexture(std::uint64_t id) noexcept
{
    return id >= kResolveSurfaceTextureBase && id < kResolveSurfaceTextureEnd;
}

// The texture and store key of a depth surface's converted R32Float view,
// tagged inside the surface id range and above every surface key.
inline constexpr std::uint64_t kResolveSurfaceDepthTag    = std::uint64_t(1) << 59;
inline constexpr std::uint64_t kResolveSurfaceDepthKeyTag = std::uint64_t(1) << 62;

constexpr std::uint64_t ResolveSurfaceDepthTexture(std::uint64_t mark) noexcept
{
    return ResolveSurfaceTexture(mark) | kResolveSurfaceDepthTag;
}

constexpr std::uint64_t ResolveSurfaceDepthKey(std::uint64_t mark) noexcept
{
    return ResolveSurfaceKey(mark) | kResolveSurfaceDepthKeyTag;
}

// The fetch conditions a surface's view rule checks, in the order and under
// the names of kNativeAliasFormNames (native_frame_assembler.h).
inline constexpr std::size_t kResolveSurfaceFormConditions = 14;

// Resolve fetch/storage view map; sampler bits are ignored. Return
//   kResolveSurfaceFormConditions or the first failed condition.
// Field/format                              | Admission or decoding fact
// Storage fields                            | Destination-equal base, size,
//   pitch, tiling, stacking, dimension and packed mips.
// Level                                     | One unsigned unbiased 2D level;
//   GetSubresourcesFromFetchConstant clamps to base.
// Mip range                                 | No mip page, or minimum = maximum
//   = 0 after clamping; k_24_8 requires only minimum = 0.
// Stored word                               | Red/blue swap then endian
//   conversion (GuestResolveStoredWord); fetch byte k reads host byte k ^
//   lanes.
// k_8_8_8_8 (6), Rgba8                       | Any packing/lanes; selectors
//   fold byte swap/permutation, except swapped Color2101010 field moves have no
//   view.
// k_2_10_10_10_AS_16_16_16_16 (54), Rgb10a2  | Packed words, lanes 0; swap only
//   for Color2101010, whose swap moves fields.
// k_16_16_FLOAT (31), Rg16Float               | Packed words, signed or not,
//   unswapped; lanes 0 or 2 exchange halves. Depth words never swap.
// k_24_8 (22), R32Float                      | Depth24Stencil8, lanes 0;
//   converted texture from convertDepthUnorm (native_texture_upload.cpp), RRRR
//   host view.
// k_24_8 observed mips                       | Base only: declared levels 1-10
//   occupy mip memory without resolve writes; sole consumer samples 1:1 at LOD
//   0 or below.
// Packed / None storage                     | Castable R32Uint / RGBA8 with
//   only its own view.
// Fetch word 1                              | Low six format bits and page base
//   (RVA-F-0126); endian/swizzle/sign follow xe_gpu_texture_fetch_t.
inline std::size_t ResolveSurfaceForm(const std::array<std::uint32_t, 6>& fetch, const GuestResolveCopy& copy, NativeGuestWordPacking packing, NativeResolvedTexture& texture)
{
    constexpr std::uint32_t kPhysicalPages = 0x1FFFF000U;
    const auto&             dest           = copy.destFetch;
    const auto              format         = fetch[1] & 0x3F;
    // The byte lane each endian mode moves lane k to is k ^ kLaneMask[mode]:
    // 8in16 swaps bytes in halves, 8in32 reverses them, 16in32 swaps halves.
    constexpr std::array<std::uint8_t, 4>        kLaneMask{ 0, 1, 3, 2 };
    const auto                                   lanes   = static_cast<std::uint8_t>(kLaneMask[copy.destInfo & 3] ^ kLaneMask[(fetch[1] >> 6) & 3]);
    const bool                                   swapped = copy.destInfo & (1U << 24);
    const bool                                   fields  = swapped && packing == NativeGuestWordPacking::Color2101010;
    const bool                                   bytes   = swapped && !fields && packing != NativeGuestWordPacking::Depth24Stencil8;
    const bool                                   words   = packing != NativeGuestWordPacking::None;
    std::optional<NativeDrawReplayTextureFormat> view;
    if (format == 6 && !fields)
        view = NativeDrawReplayTextureFormat::Rgba8;
    else if (format == 54 && words && !bytes)
        view = NativeDrawReplayTextureFormat::Rgb10a2;
    else if (format == 31 && words && !fields && !bytes)
        view = NativeDrawReplayTextureFormat::Rg16Float;
    else if (format == 22 && packing == NativeGuestWordPacking::Depth24Stencil8)
        view = NativeDrawReplayTextureFormat::R32Float;
    const bool laneView = view == NativeDrawReplayTextureFormat::Rgba8 || lanes == 0 || (view == NativeDrawReplayTextureFormat::Rg16Float && lanes == 2);
    // The SDK's mip range for the fetch: a level past the base needs a mip page.
    const auto                                                sizeMax = static_cast<std::uint32_t>(std::bit_width(std::max(fetch[2] & 8191, (fetch[2] >> 13) & 8191) + 1) - 1);
    const bool                                                mipPage = ((fetch[5] >> 12) & 0x1FFFF) != 0;
    const auto                                                lowest  = mipPage ? std::min((fetch[4] >> 2) & 15, sizeMax) : 0;
    const auto                                                highest = mipPage ? std::max(std::min((fetch[4] >> 6) & 15, sizeMax), lowest) : 0;
    const std::array<bool, kResolveSurfaceFormConditions - 1> refused{
        !view,
        (fetch[0] & 3) != 2,
        (fetch[1] & kPhysicalPages) != (copy.destBase & kPhysicalPages),
        (copy.destInfo & 7) > 3 || !laneView,
        (fetch[0] >> 22) != (dest[0] >> 22),
        (fetch[2] & 0x3FFFFFFU) != (dest[2] & 0x3FFFFFFU),
        (fetch[2] >> 26) || (fetch[1] & 0x400),
        ((fetch[5] >> 9) & 3) != 1,
        // Float formats share signed and unsigned host views; biased and
        // gamma signs, and an integer number format of a normalized one,
        // need conversions (native_texture_upload.cpp, DecodeNativeTexture).
        view == NativeDrawReplayTextureFormat::Rg16Float ? (((fetch[0] >> 2) & 0xAA) != 0) : ((fetch[0] & 0x3FC) || (fetch[3] & 1)),
        ((fetch[3] >> 13) & 63) != 0,
        lowest != 0 || (highest != 0 && view != NativeDrawReplayTextureFormat::R32Float),
        (fetch[5] & 4) != 0,
        (fetch[5] & 0x800) != (dest[5] & 0x800),
    };
    if (const auto first = std::find(refused.begin(), refused.end(), true); first != refused.end())
        return static_cast<std::size_t>(first - refused.begin());
    texture.layout = { (fetch[2] & 8191) + 1, ((fetch[2] >> 13) & 8191) + 1, *view, 1, NativeDrawReplayTextureDimension::Texture2D };
    // A data selector reads the host component holding it, as the bytes
    // path maps it (k_16_16_FLOAT's host view is RGGG, k_24_8's RRRR): the
    // data byte or half the lanes move, then the component the swap exchanged.
    constexpr std::array<std::uint8_t, 4> kSwapped{ 2, 1, 0, 3 }, kRgGG{ 0, 1, 1, 1 };
    for (std::size_t i = 0; i < texture.swizzle.size(); ++i)
    {
        const auto selector = static_cast<std::uint8_t>((fetch[3] >> (1 + 3 * i)) & 7);
        if (selector > 5)
            return kResolveSurfaceFormConditions - 1;
        if (selector >= 4)
            texture.swizzle[i] = selector;
        else if (*view == NativeDrawReplayTextureFormat::R32Float)
            texture.swizzle[i] = 0;
        else if (*view == NativeDrawReplayTextureFormat::Rg16Float)
            texture.swizzle[i] = static_cast<std::uint8_t>(kRgGG[selector] ^ (lanes >> 1));
        else if (*view == NativeDrawReplayTextureFormat::Rgba8)
            texture.swizzle[i] = bytes ? kSwapped[selector ^ lanes] : static_cast<std::uint8_t>(selector ^ lanes);
        else
            texture.swizzle[i] = fields ? kSwapped[selector] : selector;
    }
    return kResolveSurfaceFormConditions;
}

// Form misses retain copy format, packing, endian, swap, size/pitch, fetch
// fields, and first refusal condition.
struct ResolveSurfaceFormPair
{
    std::uint32_t          copyFormat = 0, copyEndian = 0, swapped = 0, width = 0, height = 0, pitch = 0, tiled = 0, packedMips = 0;
    NativeGuestWordPacking packing     = NativeGuestWordPacking::None;
    std::uint32_t          fetchFormat = 0, fetchType = 0, fetchEndian = 0, fetchWidth = 0, fetchHeight = 0, fetchPitch = 0, fetchTiled = 0, offset = 0;
    std::uint32_t          stacking = 0, dimension = 0, signs = 0, numberFormat = 0, swizzle = 0, expAdjust = 0, mips = 0, mipPage = 0, border = 0, fetchPackedMips = 0;
    std::size_t            condition = 0;

    friend auto operator<=>(const ResolveSurfaceFormPair&, const ResolveSurfaceFormPair&) = default;

    static ResolveSurfaceFormPair Of(const std::array<std::uint32_t, 6>& fetch, const GuestResolveCopy& copy, NativeGuestWordPacking packing, std::size_t condition)
    {
        ResolveSurfaceFormPair pair;
        pair.copyFormat      = (copy.destInfo >> 7) & 0x3F;
        pair.copyEndian      = copy.destInfo & 7;
        pair.swapped         = (copy.destInfo >> 24) & 1;
        pair.width           = (copy.destFetch[2] & 8191) + 1;
        pair.height          = ((copy.destFetch[2] >> 13) & 8191) + 1;
        pair.pitch           = (copy.destFetch[0] >> 22) & 0x1FF;
        pair.tiled           = copy.destFetch[0] >> 31;
        pair.packedMips      = (copy.destFetch[5] >> 11) & 1;
        pair.packing         = packing;
        pair.fetchFormat     = fetch[1] & 0x3F;
        pair.fetchType       = fetch[0] & 3;
        pair.fetchEndian     = (fetch[1] >> 6) & 3;
        pair.fetchWidth      = (fetch[2] & 8191) + 1;
        pair.fetchHeight     = ((fetch[2] >> 13) & 8191) + 1;
        pair.fetchPitch      = (fetch[0] >> 22) & 0x1FF;
        pair.fetchTiled      = fetch[0] >> 31;
        pair.stacking        = (fetch[2] >> 26) | (((fetch[1] >> 10) & 1) << 6);
        pair.offset          = (fetch[1] & 0x1FFFF000U) - (copy.destBase & 0x1FFFF000U);
        pair.dimension       = (fetch[5] >> 9) & 3;
        pair.signs           = (fetch[0] >> 2) & 0xFF;
        pair.numberFormat    = fetch[3] & 1;
        pair.swizzle         = (fetch[3] >> 1) & 0xFFF;
        pair.expAdjust       = (fetch[3] >> 13) & 63;
        pair.mips            = (fetch[4] >> 2) & 0xFF;
        pair.mipPage         = (fetch[5] >> 12) & 0x1FFFF;
        pair.border          = (fetch[5] >> 2) & 1;
        pair.fetchPackedMips = (fetch[5] >> 11) & 1;
        pair.condition       = condition;
        return pair;
    }

    // `conditions` names the conditions (kNativeAliasFormNames).
    std::string Describe(std::span<const char* const> conditions) const
    {
        static constexpr std::array<const char*, 5> kPackings{ "none", "color8888", "color2101010", "depth24stencil8", "float16x2" };
        const auto                                  packingIndex = static_cast<std::size_t>(packing);
        return std::format("resolve format={} endian={} swap={} packing={} size={}x{} pitch={} tiled={} packed_mips={} "
                           "fetch format={} type={} endian={} size={}x{} pitch={} tiled={} offset={:#x} stacking={:#x} dimension={} signs={:#04x} "
                           "number={} swizzle={:#05x} exp={} mips={:#04x} mip_page={:#x} border={} packed_mips={} refused={}",
                           copyFormat,
                           copyEndian,
                           swapped,
                           packingIndex < kPackings.size() ? kPackings[packingIndex] : "?",
                           width,
                           height,
                           pitch,
                           tiled,
                           packedMips,
                           fetchFormat,
                           fetchType,
                           fetchEndian,
                           fetchWidth,
                           fetchHeight,
                           fetchPitch,
                           fetchTiled,
                           offset,
                           stacking,
                           dimension,
                           signs,
                           numberFormat,
                           swizzle,
                           expAdjust,
                           mips,
                           mipPage,
                           border,
                           fetchPackedMips,
                           condition < conditions.size() ? conditions[condition] : "?");
    }
};

// An off-screen resolve into guest memory as the assembler recorded it.
struct ResolveSurfaceRecord
{
    std::uint64_t          mark = 0;
    GuestResolveCopy       copy;
    std::uint64_t          destEnd = 0;
    NativeGuestWordPacking packing = NativeGuestWordPacking::None;
    // A tile resolve of a predicated-tiling bracket: it takes effect when the
    // bracket ends at EndTiling 0x8269D320.
    bool staged = false;
    // Guest writes numbered at or below this predate the watch armed at record.
    std::uint64_t watchedFrom = 0;
};

// Which copy of a surface's range is current (design 2.3): the host texture
// only, or guest memory too once the resolve's image landed there.
enum class ResolveSurfaceState : std::uint8_t
{
    Host,
    Both,
};

// The texture a fetch binds, and the resolve version that produced it.
struct ResolveSurfaceBinding
{
    NativeResolvedTexture texture;
    std::uint64_t         mark = 0, version = 0;
};

using ResolveSurfaceLookup = std::variant<ResolveSurfaceBinding, NativeResolvedTextureMiss>;

// Since TakeCounts, entries=live. Count form/coverage refusals and
// unsubmitted-frame Unmodeled misses separately; a page held only by pending
// bracket version is pending, not a committed-entry miss.
struct ResolveSurfaceCounts
{
    std::uint64_t hits = 0, misses = 0, overlaps = 0, unlockedWrites = 0, refused = 0, entries = 0;
    // Older entries inside the bytes a later whole resolve writes, dropped.
    std::uint64_t droppedCovered = 0;
    // Owed tiles a later resolve, a free or a store dropped unwritten, and
    // the owed tiles left when the line was logged.
    std::uint64_t owedDropped = 0, owed = 0;
    std::uint64_t missForm = 0, missCoverage = 0, missUnmodeled = 0, missPending = 0;
    // Hits that bound a depth entry's converted k_24_8 texture.
    std::uint64_t depthViews = 0;
    // Lookups that submitted an entry's unsubmitted resolves to bind it.
    std::uint64_t submits = 0;
    // Resolve images written to guest memory for an open lock, and those
    // written for any other reason; host bytes of the images still held.
    std::uint64_t lockReadbacks = 0, flushes = 0, heldBytes = 0;
};

// How the ledger's take of the resolve image at a mark proceeds.
enum class ResolveSurfaceHold : std::uint8_t
{
    Write,  // no live entry holds the mark: the image lands now
    Hold,   // a live entry's tile with no open lock over it: it stays on the host
    Locked, // a live entry's tile under an open lock: it lands now, as the lock's readback
};

// The ranges one guest lock names.
using ResolveSurfaceLockRanges = std::array<std::pair<std::uint64_t, std::uint64_t>, 2>;

// The `Native frame timing:` fields of `counts`, each after a space.
inline std::string FormatResolveSurfaceCounts(const ResolveSurfaceCounts& counts)
{
    return std::format(" surface_hits={} surface_misses={} surface_miss_form={} surface_miss_coverage={} surface_miss_unmodeled={} surface_miss_pending={} surface_overlaps={} surface_dropped_covered={} surface_owed_dropped={} surface_owed={} surface_unlocked_writes={} surface_refused={} surface_entries={} surface_depth_views={} surface_submits={} lock_readbacks={} surface_flushes={} surface_readback_mb={:.1f}",
                       counts.hits,
                       counts.misses,
                       counts.missForm,
                       counts.missCoverage,
                       counts.missUnmodeled,
                       counts.missPending,
                       counts.overlaps,
                       counts.droppedCovered,
                       counts.owedDropped,
                       counts.owed,
                       counts.unlockedWrites,
                       counts.refused,
                       counts.entries,
                       counts.depthViews,
                       counts.submits,
                       counts.lockReadbacks,
                       counts.flushes,
                       double(counts.heldBytes) / double(1 << 20));
}

// Off-screen physical-range host versions outlive epochs/chains. Covering
// resolve supersedes, other overlap ends old entry. Pending bracket tiles
// assemble in resolve order, later rect wins; publish at bracket end.
class ResolveSurfaceRegistry
{
public:
    // Live destinations beyond this are refused, not evicted: the refused
    // resolve ends the entries it overlaps and its readers take the bytes path.
    static constexpr std::size_t kEntries = 64;
    // Tile resolves one pending version holds; a further one is refused and
    // ends the version.
    static constexpr std::size_t kTiles = 64;
    // Open locks beyond this are forgotten, oldest first by lock order.
    static constexpr std::size_t kLocks = 1024;
    // Distinct form-miss pairs noted; later ones are not.
    static constexpr std::size_t kFormPairs = 256;
    // Distinct converted depth texture consumers noted; later ones are not.
    static constexpr std::size_t kDepthConsumers = 64;
    // Ended entries whose images stay owed; a further one makes the oldest
    // owed now.
    static constexpr std::size_t kOwed = 64;

    struct EntryView
    {
        std::uint64_t       version   = 0;
        ResolveSurfaceState state     = ResolveSurfaceState::Host;
        bool                committed = false, admitted = false;
        // The assembled destination's size; 0 when the resolve's own texture
        // is the surface.
        std::uint32_t width = 0, height = 0;
    };

    void Record(const ResolveSurfaceRecord& record)
    {
        std::lock_guard lock(mutex);
        const Tile      tile{ record };
        const auto      begin = std::uint64_t(record.copy.destBase), end = begin + (record.destEnd - record.copy.destBase);
        const auto      pending = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry)
                                               {
                                              return !entry.committed && overlaps(entry, begin, end);
                                               });
        // A tile of the pending version's destination joins it, after the
        // tiles before it; the version waits for its frame and image again.
        if (pending != entries.end() && record.staged && pending->width && sameDestination(pending->record, record) && fits(*pending, tile))
        {
            // The refused tile still writes guest memory: every entry over
            // its range ends, the version with it.
            if (pending->tiles.size() >= kTiles)
            {
                refusedCount.fetch_add(1, std::memory_order_relaxed);
                endWhere([&](const Entry& entry)
                         {
                             return overlaps(entry, begin, end);
                         });
                return;
            }
            pending->tiles.push_back(tile);
            pending->admitted = false;
            pending->state    = ResolveSurfaceState::Host;
            pending->complete = writes(*pending, 0, 0, pending->width, pending->height);
            return;
        }
        // Any other resolve is newer than every tile of the pending version
        // it overlaps, which ends.
        endWhere(
            [&](const Entry& entry)
            {
                if (entry.committed || !overlaps(entry, begin, end))
                    return false;
                overlapCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            },
            { begin, end });
        if (entries.size() >= kEntries)
        {
            refusedCount.fetch_add(1, std::memory_order_relaxed);
            endWhere([&](const Entry& entry)
                     {
                         return overlaps(entry, begin, end);
                     });
            return;
        }
        Entry entry{ record };
        entry.tiles.push_back(tile);
        // A tile whose destination fetch gives the destination's size
        // assembles it; others stay their own texture, a coverage miss.
        if (const auto& dest = record.copy.destFetch; record.staged && (dest[0] & 3) == 2)
        {
            entry.width  = (dest[2] & 8191) + 1;
            entry.height = ((dest[2] >> 13) & 8191) + 1;
            if (!fits(entry, tile))
                entry.width = entry.height = 0;
            entry.complete = entry.width && writes(entry, 0, 0, entry.width, entry.height);
        }
        entries.push_back(std::move(entry));
        if (!record.staged)
            commit(record.mark);
    }

    // The predicated-tiling bracket ended: its pending versions take effect,
    // so a draw inside it bound the versions before it opened. Pending
    // versions do not overlap, so their order does not matter.
    void CommitTiles()
    {
        std::lock_guard            lock(mutex);
        std::vector<std::uint64_t> pending;
        for (const auto& entry : entries)
            if (!entry.committed)
                pending.push_back(entry.record.mark);
        // By mark: a commit ends older entries, which moves the others.
        for (const auto mark : pending)
            commit(mark);
    }

    // The frame holding the resolve at `mark`'s host texture was submitted;
    // `stored` is that texture as a later frame imports it. An assembled
    // version is admitted once every tile's frame was, as its texture.
    void Admit(std::uint64_t mark, NativeRenderFrameTexture stored)
    {
        std::lock_guard lock(mutex);
        auto*           entry = live(mark);
        if (!entry)
            return;
        tileOf(*entry, mark)->admitted = true;
        if (entry->width)
        {
            stored.id            = ResolveSurfaceTexture(entry->record.mark);
            stored.persistentKey = ResolveSurfaceKey(entry->record.mark);
            stored.width         = entry->width;
            stored.height        = entry->height;
        }
        entry->stored   = stored;
        entry->admitted = std::all_of(entry->tiles.begin(), entry->tiles.end(), [](const Tile& tile)
                                      {
                                          return tile.admitted;
                                      });
    }

    // The ledger wrote the resolve's image to guest memory; guest memory is
    // current once every tile's image landed. A skipped image never lands.
    void Landed(std::uint64_t mark)
    {
        std::lock_guard lock(mutex);
        forgetOwed(mark);
        auto* entry = live(mark);
        if (!entry)
            return;
        tileOf(*entry, mark)->landed = true;
        entry->state                 = std::all_of(entry->tiles.begin(), entry->tiles.end(), [](const Tile& tile)
                                                   {
                                       return tile.landed;
                                                   })
                                           ? ResolveSurfaceState::Both
                                           : ResolveSurfaceState::Host;
    }

    // Unmodeled guest writes end overlapping entries and make their images owed
    // unless the store is landing over them.
    void Overwritten(std::uint64_t begin, std::uint64_t end, bool owe = true)
    {
        std::lock_guard lock(mutex);
        const auto      over = [&](const Entry& entry)
        {
            return overlaps(entry, begin, end);
        };
        endOwed(over, owe ? Ending::OwedNow : Ending::Dropped);
        endWhere(over, {}, owe ? Ending::OwedNow : Ending::Dropped);
    }

    // The physical allocation [begin, end) was freed: its entries drop
    // without a readback, as the guest no longer owns the memory.
    void Freed(std::uint64_t begin, std::uint64_t end)
    {
        Overwritten(begin, end, false);
    }

    // Skipped image leaves old guest bytes, ending its entry. A pending version
    // may still land other tiles later, so older entries over that range end
    // with it now.
    void Skipped(std::uint64_t mark)
    {
        std::lock_guard lock(mutex);
        forgetOwed(mark);
        if (auto* entry = live(mark))
            tileOf(*entry, mark)->skipped = true;
        endWhere([&](const Entry& entry)
                 {
                     return std::any_of(entry.tiles.begin(), entry.tiles.end(), [&](const Tile& tile)
                                        {
                                            return tile.record.mark == mark;
                                        });
                 });
    }

    // The marks the ended entries' tiles named since the previous call:
    // images guest memory still owes, and images it no longer needs.
    struct Endings
    {
        std::vector<std::uint64_t> owed, dropped;
    };

    Endings TakeEndings()
    {
        std::lock_guard lock(mutex);
        return std::exchange(endings, {});
    }

    // Dropped image endings remain queued or handled under the same owner lock
    // that reads them.
    std::vector<std::uint64_t> PendingDrops() const
    {
        std::lock_guard lock(mutex);
        return endings.dropped;
    }

    // Whether the ledger may keep the resolve image at `mark` on the host: a
    // live entry's or an owed one's tile.
    ResolveSurfaceHold Holds(std::uint64_t mark) const
    {
        std::lock_guard lock(mutex);
        for (const auto* list : { &entries, &owed })
            for (const auto& entry : *list)
                for (const auto& tile : entry.tiles)
                    if (tile.record.mark == mark)
                        return lockedAny(entry) ? ResolveSurfaceHold::Locked : ResolveSurfaceHold::Hold;
        return ResolveSurfaceHold::Write;
    }

    // The marks of the live entries' tiles, pending versions included, and
    // the owed tiles that `ranges` overlap.
    std::vector<std::uint64_t> LockedMarks(const ResolveSurfaceLockRanges& ranges) const
    {
        std::lock_guard            lock(mutex);
        std::vector<std::uint64_t> marks;
        for (const auto* list : { &entries, &owed })
            for (const auto& entry : *list)
                if (std::any_of(ranges.begin(), ranges.end(), [&](const auto& range)
                                {
                                    return range.first < range.second && overlaps(entry, range.first, range.second);
                                }))
                    for (const auto& tile : entry.tiles)
                        marks.push_back(tile.record.mark);
        return marks;
    }

    // SetSettle installs/removes held-image owner. Settle invokes it outside
    // the registry lock for newly locked ranges and owed endings; removal waits
    // for in-flight call.
    void SetSettle(std::function<void(const ResolveSurfaceLockRanges*)> owner)
    {
        std::lock_guard serialized(settleMutex);
        settle = std::move(owner);
        std::lock_guard lock(mutex);
        trackEndings = settle != nullptr;
        if (!trackEndings)
        {
            endings = {};
            owed.clear();
        }
    }

    // Runs the owner, serialized; call without the owner's locks.
    void Settle(const ResolveSurfaceLockRanges* locked = nullptr)
    {
        std::lock_guard serialized(settleMutex);
        if (settle)
            settle(locked);
    }

    void CountLockReadbacks(std::uint64_t images) noexcept
    {
        lockReadbacks.fetch_add(images, std::memory_order_relaxed);
    }

    void CountFlushes(std::uint64_t images) noexcept
    {
        flushes.fetch_add(images, std::memory_order_relaxed);
    }

    void SetHeldBytes(std::uint64_t bytes) noexcept
    {
        heldBytes.store(bytes, std::memory_order_relaxed);
    }

    // A lock of `object` naming [begin, end) ranges opened; `write` when it
    // may write them.
    void Locked(std::uint32_t object, const ResolveSurfaceLockRanges& ranges, bool write)
    {
        std::lock_guard lock(mutex);
        auto&           open = locks[object];
        if (!open.depth)
        {
            open.ranges = ranges;
            open.order  = ++lockOrder;
        }
        ++open.depth;
        open.write |= write;
        if (locks.size() > kLocks)
        {
            const auto oldest = std::min_element(locks.begin(), locks.end(), [](const auto& left, const auto& right)
                                                 {
                                                     return left.second.order < right.second.order;
                                                 });
            locks.erase(oldest);
        }
    }

    // The last unlock of `object` closed: a write lock ends the entries its
    // ranges overlap, as do stores the watch saw while it was open.
    void Unlocked(std::uint32_t object)
    {
        std::lock_guard lock(mutex);
        const auto      found = locks.find(object);
        if (found == locks.end() || --found->second.depth)
            return;
        const auto open = found->second;
        locks.erase(found);
        endWhere([&](const Entry& entry)
                 {
                     bool hit = false;
                     for (const auto& [begin, end] : open.ranges)
                         hit |= begin < end && overlaps(entry, begin, end);
                     return hit && (open.write || entry.endAtUnlock);
                 });
    }

    // A guest store the write watch reported as `number` over [begin, end).
    // Inside an open write lock of the range it ends the entry at that
    // unlock; otherwise it ends the entry now and counts an unlocked write.
    void Written(std::uint64_t begin, std::uint64_t end, std::uint64_t number)
    {
        std::lock_guard lock(mutex);
        for (auto& entry : entries)
            if (overlaps(entry, begin, end) && number > entry.record.watchedFrom && writeLocked(entry))
                entry.endAtUnlock = true;
        // The notice precedes the store, so a later write of the image would
        // overwrite it: the images drop. Owed tiles under an open write lock
        // landed when it opened, or land when taken.
        endOwed(
            [&](const Entry& entry)
            {
                return overlaps(entry, begin, end) && number > entry.record.watchedFrom && !writeLocked(entry);
            },
            Ending::Dropped);
        endWhere(
            [&](const Entry& entry)
            {
                if (!overlaps(entry, begin, end) || number <= entry.record.watchedFrom || entry.endAtUnlock)
                    return false;
                unlockedWrites.fetch_add(1, std::memory_order_relaxed);
                return true;
            },
            {},
            Ending::Dropped);
    }

    // Find committed fetch-base holder passing view rule; none=nullopt, pending
    // flags pending-only holder. Committed ranges do not overlap.
    // formRefusal/pair names miss; unsubmitted=newest unadmitted mark.
    std::optional<ResolveSurfaceLookup> Find(const std::array<std::uint32_t, 6>& fetch, bool castable, std::size_t* formRefusal = nullptr, ResolveSurfaceFormPair* pair = nullptr, bool* pending = nullptr, std::uint64_t* unsubmitted = nullptr) const
    {
        std::lock_guard     lock(mutex);
        const std::uint64_t base  = fetch[1] & 0x1FFFF000U;
        const auto          holds = [&](const Entry& entry)
        {
            return base >= (entry.record.copy.destBase & 0x1FFFF000U) && base < masked(entry.record.destEnd, entry.record.copy.destBase);
        };
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
        {
            const auto& record = entry->record;
            if (!entry->committed || !holds(*entry))
                continue;
            ResolveSurfaceBinding binding;
            if (const auto failed = ResolveSurfaceForm(fetch, record.copy, record.packing, binding.texture); failed < kResolveSurfaceFormConditions)
            {
                if (formRefusal)
                    *formRefusal = failed;
                if (pair)
                    *pair = ResolveSurfaceFormPair::Of(fetch, record.copy, record.packing, failed);
                return NativeResolvedTextureMiss::Form;
            }
            if (!entry->admitted)
            {
                if (unsubmitted)
                    for (const auto& tile : entry->tiles)
                        if (!tile.admitted)
                            *unsubmitted = std::max(*unsubmitted, tile.record.mark);
                return NativeResolvedTextureMiss::Unmodeled;
            }
            const auto& layout = binding.texture.layout;
            if (!covered(*entry, layout.width, layout.height))
                return NativeResolvedTextureMiss::Coverage;
            // The converted depth texture is R32Float storage, read uncast.
            const bool converted = layout.format == NativeDrawReplayTextureFormat::R32Float;
            if (record.packing != NativeGuestWordPacking::None && !castable && !converted)
                return NativeResolvedTextureMiss::Packing;
            binding.texture.frameTextureId = converted ? ResolveSurfaceDepthTexture(record.mark) : ResolveSurfaceTexture(record.mark);
            binding.mark                   = record.mark;
            binding.version                = entry->version;
            return binding;
        }
        if (pending)
            *pending = std::any_of(entries.begin(), entries.end(), [&](const Entry& entry)
                                   {
                                       return !entry.committed && holds(entry);
                                   });
        return std::nullopt;
    }

    // The import a frame declares for a draw that binds `textureId`, while
    // the entry lives or its key waits for release: the stored texture, or a
    // depth entry's converted R32Float texture of the same size.
    std::optional<NativeRenderFrameTexture> Import(std::uint64_t textureId) const
    {
        std::lock_guard lock(mutex);
        for (const auto* list : { &entries, &retiring })
            for (const auto& entry : *list)
            {
                if (!entry.admitted)
                    continue;
                if (entry.stored.id == textureId)
                    return entry.stored;
                if (entry.record.packing == NativeGuestWordPacking::Depth24Stencil8 && ResolveSurfaceDepthTexture(entry.record.mark) == textureId)
                {
                    auto converted          = entry.stored;
                    converted.id            = textureId;
                    converted.format        = NativeDrawReplayTextureFormat::R32Float;
                    converted.persistentKey = ResolveSurfaceDepthKey(entry.record.mark);
                    converted.importedWords = NativeGuestWordPacking::None;
                    return converted;
                }
            }
        return std::nullopt;
    }

    // Resolved texture returns nullopt after store-key release or resolve
    // refusal.
    std::optional<NativeRenderFrameSurface> FrameSurface(std::uint64_t mark, std::uint64_t textureId) const
    {
        std::lock_guard lock(mutex);
        for (const auto* list : { &entries, &retiring })
            for (const auto& entry : *list)
                for (const auto& tile : entry.tiles)
                    if (tile.record.mark == mark)
                    {
                        NativeRenderFrameSurface surface{ textureId, ResolveSurfaceKey(entry.record.mark) };
                        // Depth words also write their k_24_8 conversion.
                        if (entry.record.packing == NativeGuestWordPacking::Depth24Stencil8)
                            surface.depthKey = ResolveSurfaceDepthKey(entry.record.mark);
                        if (entry.width)
                        {
                            surface.x      = tile.record.copy.destX;
                            surface.y      = tile.record.copy.destY;
                            surface.width  = entry.width;
                            surface.height = entry.height;
                        }
                        return surface;
                    }
        return std::nullopt;
    }

    // The store keys of ended entries no draw still to be replayed binds,
    // a depth entry's converted texture with its own; `referenced` names the
    // frame texture ids such draws hold.
    std::vector<std::uint64_t> TakeReleases(const std::function<bool(std::uint64_t)>& referenced)
    {
        std::lock_guard            lock(mutex);
        std::vector<std::uint64_t> keys;
        std::erase_if(retiring, [&](const Entry& entry)
                      {
                          const auto mark  = entry.record.mark;
                          const bool depth = entry.record.packing == NativeGuestWordPacking::Depth24Stencil8;
                          if (referenced && (referenced(ResolveSurfaceTexture(mark)) || (depth && referenced(ResolveSurfaceDepthTexture(mark)))))
                              return false;
                          keys.push_back(ResolveSurfaceKey(mark));
                          if (depth)
                              keys.push_back(ResolveSurfaceDepthKey(mark));
                          return true;
                      });
        return keys;
    }

    // The resolves whose images a lock of [begin, end) must read back first,
    // in resolve order: the tiles of the newest committed, admitted entry
    // overlapping it whose range is Host only.
    std::vector<ResolveSurfaceRecord> HostCopy(std::uint64_t begin, std::uint64_t end) const
    {
        std::lock_guard                   lock(mutex);
        std::vector<ResolveSurfaceRecord> records;
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
            if (entry->committed && entry->admitted && entry->state == ResolveSurfaceState::Host && overlaps(*entry, begin, end))
            {
                for (const auto& tile : entry->tiles)
                    records.push_back(tile.record);
                break;
            }
        return records;
    }

    // The newest live entry overlapping [begin, end): its first resolve's
    // mark and its destination range, for native_frame_timing's read split.
    std::optional<std::array<std::uint64_t, 3>> Overlapping(std::uint64_t begin, std::uint64_t end) const
    {
        std::lock_guard lock(mutex);
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
            if (overlaps(*entry, begin, end))
                return std::array<std::uint64_t, 3>{ entry->record.mark, physical(entry->record.copy.destBase), masked(entry->record.destEnd, entry->record.copy.destBase) };
        return std::nullopt;
    }

    // A live entry's version and state by any of its resolves' marks;
    // nullopt once it ended.
    std::optional<EntryView> View(std::uint64_t mark) const
    {
        std::lock_guard lock(mutex);
        for (const auto& entry : entries)
            for (const auto& tile : entry.tiles)
                if (tile.record.mark == mark)
                    return EntryView{ entry.version, entry.state, entry.committed, entry.admitted, entry.width, entry.height };
        return std::nullopt;
    }

    void CountLookup(bool hit) noexcept
    {
        (hit ? hits : misses).fetch_add(1, std::memory_order_relaxed);
    }

    // A miss's reason, besides CountLookup(false); a pending page counts
    // only here.
    void CountMiss(NativeResolvedTextureMiss miss) noexcept
    {
        if (miss == NativeResolvedTextureMiss::Form)
            missForm.fetch_add(1, std::memory_order_relaxed);
        else if (miss == NativeResolvedTextureMiss::Coverage)
            missCoverage.fetch_add(1, std::memory_order_relaxed);
        else if (miss == NativeResolvedTextureMiss::Unmodeled)
            missUnmodeled.fetch_add(1, std::memory_order_relaxed);
    }

    void CountPending() noexcept
    {
        missPending.fetch_add(1, std::memory_order_relaxed);
    }

    // A lookup that submitted the entry's resolves before binding it.
    void CountSubmit() noexcept
    {
        submits.fetch_add(1, std::memory_order_relaxed);
    }

    // A hit that bound a converted depth texture, besides CountLookup(true).
    void CountDepthView() noexcept
    {
        depthViews.fetch_add(1, std::memory_order_relaxed);
    }

    // Converted depth textures keep base level only, so bytes-path consumers
    // may sample levels unavailable to the converter.
    bool NoteDepthConsumer(std::uint64_t pixelShader, std::uint32_t caller)
    {
        std::lock_guard lock(mutex);
        if (depthConsumers.size() >= kDepthConsumers || !depthConsumers.emplace(pixelShader, caller).second)
            return false;
        newDepthConsumers.emplace_back(pixelShader, caller);
        return true;
    }

    // The (pixel shader, caller) pairs noted for the first time since the
    // previous call.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> TakeNewDepthConsumers()
    {
        std::lock_guard lock(mutex);
        return std::exchange(newDepthConsumers, {});
    }

    // Counts a form miss's shapes; true the first time this pair is noted,
    // when it also waits for TakeNewFormPairs.
    bool NoteFormPair(const ResolveSurfaceFormPair& pair)
    {
        std::lock_guard lock(mutex);
        if (const auto found = formPairs.find(pair); found != formPairs.end())
        {
            ++found->second;
            return false;
        }
        if (formPairs.size() >= kFormPairs)
            return false;
        formPairs.emplace(pair, 1);
        newFormPairs.push_back(pair);
        return true;
    }

    // The pairs noted for the first time since the previous call.
    std::vector<ResolveSurfaceFormPair> TakeNewFormPairs()
    {
        std::lock_guard lock(mutex);
        return std::exchange(newFormPairs, {});
    }

    // Every pair noted, with the form misses counted under it.
    std::vector<std::pair<ResolveSurfaceFormPair, std::uint64_t>> FormPairs() const
    {
        std::lock_guard lock(mutex);
        return { formPairs.begin(), formPairs.end() };
    }

    ResolveSurfaceCounts TakeCounts()
    {
        ResolveSurfaceCounts counts;
        counts.hits           = hits.exchange(0, std::memory_order_relaxed);
        counts.misses         = misses.exchange(0, std::memory_order_relaxed);
        counts.overlaps       = overlapCount.exchange(0, std::memory_order_relaxed);
        counts.droppedCovered = droppedCovered.exchange(0, std::memory_order_relaxed);
        counts.owedDropped    = owedDropped.exchange(0, std::memory_order_relaxed);
        counts.unlockedWrites = unlockedWrites.exchange(0, std::memory_order_relaxed);
        counts.refused        = refusedCount.exchange(0, std::memory_order_relaxed);
        counts.missForm       = missForm.exchange(0, std::memory_order_relaxed);
        counts.missCoverage   = missCoverage.exchange(0, std::memory_order_relaxed);
        counts.missUnmodeled  = missUnmodeled.exchange(0, std::memory_order_relaxed);
        counts.missPending    = missPending.exchange(0, std::memory_order_relaxed);
        counts.depthViews     = depthViews.exchange(0, std::memory_order_relaxed);
        counts.submits        = submits.exchange(0, std::memory_order_relaxed);
        counts.lockReadbacks  = lockReadbacks.exchange(0, std::memory_order_relaxed);
        counts.flushes        = flushes.exchange(0, std::memory_order_relaxed);
        counts.heldBytes      = heldBytes.load(std::memory_order_relaxed);
        std::lock_guard lock(mutex);
        counts.entries = entries.size();
        for (const auto& entry : owed)
            counts.owed += entry.tiles.size();
        return counts;
    }

    std::size_t Size() const
    {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    // Runs before a guest transition that a queued draw could observe, so
    // deferred draws drain first (native_draw_sequencer); null removes it.
    void SetBarrier(std::function<void()> barrier)
    {
        std::lock_guard lock(barrierMutex);
        this->barrier = std::move(barrier);
    }

    // Runs the barrier; call without the registry's locks.
    void Barrier() const
    {
        std::function<void()> run;
        {
            std::lock_guard lock(barrierMutex);
            run = barrier;
        }
        if (run)
            run();
    }

    // Whether a live or owed entry overlaps one of `ranges`.
    bool Touches(const ResolveSurfaceLockRanges& ranges) const
    {
        std::lock_guard lock(mutex);
        for (const auto* list : { &entries, &owed })
            for (const auto& entry : *list)
                for (const auto& [begin, end] : ranges)
                    if (begin < end && overlaps(entry, begin, end))
                        return true;
        return false;
    }

    // Whether the next unlock of `object` closes its lock over an entry.
    bool UnlockTouches(std::uint32_t object) const
    {
        ResolveSurfaceLockRanges ranges{};
        {
            std::lock_guard lock(mutex);
            const auto      found = locks.find(object);
            if (found == locks.end() || found->second.depth != 1)
                return false;
            ranges = found->second.ranges;
        }
        return Touches(ranges);
    }

    // Whether EndTiling has a pending version to commit.
    bool TilesPending() const
    {
        std::lock_guard lock(mutex);
        return std::any_of(entries.begin(), entries.end(), [](const Entry& entry)
                           {
                               return !entry.committed;
                           });
    }

private:
    // One resolve writing a version, at its destination rectangle.
    struct Tile
    {
        ResolveSurfaceRecord record;
        bool                 admitted = false, landed = false;
        // The ledger skipped its image, which never lands: no ending owes it.
        bool skipped = false;
    };

    struct Entry
    {
        // The version's first resolve, whose mark names its texture and key.
        ResolveSurfaceRecord record;
        std::vector<Tile>    tiles;
        // The assembled destination's size; 0 when the first resolve's own
        // texture is the surface.
        std::uint32_t       width = 0, height = 0;
        std::uint64_t       version   = 1;
        ResolveSurfaceState state     = ResolveSurfaceState::Host;
        bool                committed = false, admitted = false, endAtUnlock = false;
        // An assembled destination its tiles write whole.
        bool                     complete = false;
        NativeRenderFrameTexture stored;
    };

    struct Lock
    {
        std::array<std::pair<std::uint64_t, std::uint64_t>, 2> ranges{};
        std::uint32_t                                          depth = 0;
        std::uint64_t                                          order = 0;
        bool                                                   write = false;
    };

    mutable std::mutex mutex;
    // Draws bind committed version until bracket commit. Pending versions never
    // overlap; any other overlapping resolve ends them. Write-intent unlock,
    // outside store, unmodeled write, skipped image or free ends an entry.
    std::vector<Entry> entries, retiring;
    // Ended entries, oldest first, holding only their owed tiles.
    std::vector<Entry> owed;
    // Ended keys wait frame release. Covering next/whole resolves, free or
    // landing stores drop images; unmodeled writes owe now. Other unlanded
    // tiles stay host-owned until needed or kOwed overflow.
    std::unordered_map<std::uint32_t, Lock>              locks;
    std::uint64_t                                        lockOrder = 0;
    std::atomic<std::uint64_t>                           hits{ 0 }, misses{ 0 }, overlapCount{ 0 }, unlockedWrites{ 0 }, refusedCount{ 0 };
    std::atomic<std::uint64_t>                           missForm{ 0 }, missCoverage{ 0 }, missUnmodeled{ 0 }, missPending{ 0 };
    std::map<ResolveSurfaceFormPair, std::uint64_t>      formPairs;
    std::vector<ResolveSurfaceFormPair>                  newFormPairs;
    std::atomic<std::uint64_t>                           depthViews{ 0 }, submits{ 0 }, droppedCovered{ 0 }, owedDropped{ 0 };
    std::map<std::uint64_t, std::uint32_t>               depthConsumers;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> newDepthConsumers;
    std::atomic<std::uint64_t>                           lockReadbacks{ 0 }, flushes{ 0 }, heldBytes{ 0 };
    Endings                                              endings;
    bool                                                 trackEndings = false;
    // Serializes Settle with SetSettle; held around the owner, never inside mutex.
    std::mutex settleMutex;
    // Registry mutex is a leaf; Settle invokes owner outside it. Owed images
    // serve no fetch but remain until lock/read/newer image needs them,
    // coverage/store frees them, or later owed bound forces settlement.
    std::function<void(const ResolveSurfaceLockRanges*)> settle;
    mutable std::mutex                                   barrierMutex;
    std::function<void()>                                barrier;

    // Guest physical addresses under the guest's 512 MB physical mask.
    static std::uint64_t physical(std::uint64_t address) noexcept
    {
        return address & 0x1FFFFFFFU;
    }

    static std::uint64_t masked(std::uint64_t end, std::uint32_t base) noexcept
    {
        return physical(base) + (end - base);
    }

    static bool overlaps(const Entry& entry, std::uint64_t begin, std::uint64_t end) noexcept
    {
        const auto first = physical(entry.record.copy.destBase), last = masked(entry.record.destEnd, entry.record.copy.destBase);
        const auto from = physical(begin);
        return from < last && first < from + (end - begin);
    }

    static bool sameRange(const ResolveSurfaceRecord& left, const ResolveSurfaceRecord& right) noexcept
    {
        return physical(left.copy.destBase) == physical(right.copy.destBase) && left.destEnd - left.copy.destBase == right.destEnd - right.copy.destBase;
    }

    // The destination storage and layout two resolves write.
    static bool sameDestination(const ResolveSurfaceRecord& left, const ResolveSurfaceRecord& right) noexcept
    {
        return sameRange(left, right) && left.copy.destPitch == right.copy.destPitch && left.copy.destInfo == right.copy.destInfo &&
               left.copy.destFetch == right.copy.destFetch && left.packing == right.packing;
    }

    static bool fits(const Entry& entry, const Tile& tile) noexcept
    {
        const auto& copy = tile.record.copy;
        return copy.rectWidth && copy.rectHeight && std::uint64_t(copy.destX) + copy.rectWidth <= entry.width && std::uint64_t(copy.destY) + copy.rectHeight <= entry.height;
    }

    // Whether the union of `entry`'s rectangles, which may overlap, writes
    // all of [x0, x1) x [y0, y1): every cell of the grid their edges cut it
    // into lies inside one of them. At most kTiles rectangles.
    static bool writes(const Entry& entry, std::uint64_t x0, std::uint64_t y0, std::uint64_t x1, std::uint64_t y1)
    {
        std::vector<std::uint64_t> xs{ x0, x1 }, ys{ y0, y1 };
        for (const auto& tile : entry.tiles)
        {
            const auto& copy = tile.record.copy;
            xs.push_back(std::clamp<std::uint64_t>(copy.destX, x0, x1));
            xs.push_back(std::clamp<std::uint64_t>(std::uint64_t(copy.destX) + copy.rectWidth, x0, x1));
            ys.push_back(std::clamp<std::uint64_t>(copy.destY, y0, y1));
            ys.push_back(std::clamp<std::uint64_t>(std::uint64_t(copy.destY) + copy.rectHeight, y0, y1));
        }
        for (auto* edges : { &xs, &ys })
        {
            std::sort(edges->begin(), edges->end());
            edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
        }
        for (std::size_t i = 0; i + 1 < xs.size(); ++i)
            for (std::size_t j = 0; j + 1 < ys.size(); ++j)
                if (std::none_of(entry.tiles.begin(), entry.tiles.end(), [&](const Tile& tile)
                                 {
                                     const auto& copy = tile.record.copy;
                                     return copy.destX <= xs[i] && std::uint64_t(copy.destX) + copy.rectWidth >= xs[i + 1] && copy.destY <= ys[j] &&
                                            std::uint64_t(copy.destY) + copy.rectHeight >= ys[j + 1];
                                 }))
                    return false;
        return true;
    }

    // Whether a fetch of width x height reads only texels the entry's
    // texture holds: an assembled destination of that size its tiles cover,
    // or one resolve of that size at the origin.
    static bool covered(const Entry& entry, std::uint32_t width, std::uint32_t height) noexcept
    {
        if (entry.width)
            return entry.width == width && entry.height == height && entry.complete;
        const auto& copy = entry.record.copy;
        return !copy.destX && !copy.destY && copy.rectWidth == width && copy.rectHeight == height;
    }

    // Whether `newer` writes every rectangle `older` wrote.
    static bool covers(const Entry& newer, const Entry& older)
    {
        return std::all_of(older.tiles.begin(), older.tiles.end(), [&](const Tile& tile)
                           {
                               const auto& copy = tile.record.copy;
                               return writes(newer, copy.destX, copy.destY, std::uint64_t(copy.destX) + copy.rectWidth, std::uint64_t(copy.destY) + copy.rectHeight);
                           });
    }

    // Proven full-overwrite byte extent only: whole 32-row tile rows across
    // full pitch from origin, 32-bit stores. Return 0 for other rectangles or
    // refused array/volume forms; 0 proves no full overwrite.
    static std::uint64_t wholeBytes(const ResolveSurfaceRecord& record) noexcept
    {
        const auto& copy  = record.copy;
        const auto  pitch = copy.destPitch & 0x3FFF;
        if ((copy.destInfo & 0x78) || copy.destX || copy.destY || !pitch || pitch % 32 || copy.rectWidth != pitch || !copy.rectHeight || copy.rectHeight % 32)
            return 0;
        return std::min<std::uint64_t>(std::uint64_t(pitch) * copy.rectHeight * 4, record.destEnd - copy.destBase);
    }

    // Whether a tile of `newer`, later than every tile of `older`, writes
    // every byte of `older`'s range.
    static bool wholeOver(const Entry& newer, const Entry& older)
    {
        const auto first = physical(older.record.copy.destBase), last = masked(older.record.destEnd, older.record.copy.destBase);
        return std::any_of(newer.tiles.begin(), newer.tiles.end(), [&](const Tile& tile)
                           {
                               const auto& record = tile.record;
                               const auto  base   = physical(record.copy.destBase);
                               return base <= first && last <= base + wholeBytes(record) &&
                                      std::all_of(older.tiles.begin(), older.tiles.end(), [&](const Tile& old)
                                                  {
                                                      return old.record.mark < record.mark;
                                                  });
                           });
    }

    // Whether an open lock of any intent names the entry's range.
    bool lockedAny(const Entry& entry) const
    {
        for (const auto& [object, open] : locks)
            for (const auto& [begin, end] : open.ranges)
                if (begin < end && overlaps(entry, begin, end))
                    return true;
        return false;
    }

    // How an ending treats the entry's images.
    enum class Ending : std::uint8_t
    {
        Dropped, // guest memory no longer needs them
        Owed,    // its unlanded tiles stay owed
        OwedNow, // they land now
    };

    // Under mutex: names the ended entry's tiles for TakeEndings, or keeps
    // its unlanded tiles owed; past kOwed the oldest owed tiles land now.
    void noteEnded(const Entry& entry, Ending ending)
    {
        if (!trackEndings)
            return;
        if (ending != Ending::Owed)
        {
            auto& marks = ending == Ending::Dropped ? endings.dropped : endings.owed;
            for (const auto& tile : entry.tiles)
                marks.push_back(tile.record.mark);
            return;
        }
        Entry kept;
        kept.record = entry.record;
        kept.width  = entry.width;
        kept.height = entry.height;
        for (const auto& tile : entry.tiles)
            if (!tile.landed && !tile.skipped)
                kept.tiles.push_back(tile);
        if (kept.tiles.empty())
            return;
        owed.push_back(std::move(kept));
        if (owed.size() > kOwed)
        {
            for (const auto& tile : owed.front().tiles)
                endings.owed.push_back(tile.record.mark);
            owed.erase(owed.begin());
        }
    }

    // Under mutex: ends the owed tiles of the entries `predicate` names,
    // dropped (counted) or owed now.
    template <typename Predicate>
    void endOwed(Predicate&& predicate, Ending ending)
    {
        std::erase_if(owed, [&](const Entry& entry)
                      {
                          if (!predicate(entry))
                              return false;
                          if (ending == Ending::Dropped)
                              owedDropped.fetch_add(entry.tiles.size(), std::memory_order_relaxed);
                          auto& marks = ending == Ending::Dropped ? endings.dropped : endings.owed;
                          for (const auto& tile : entry.tiles)
                              marks.push_back(tile.record.mark);
                          return true;
                      });
    }

    // Under mutex: the image at `mark` landed or was skipped, so no tile owes it.
    void forgetOwed(std::uint64_t mark)
    {
        for (auto& entry : owed)
            std::erase_if(entry.tiles, [&](const Tile& tile)
                          {
                              return tile.record.mark == mark;
                          });
        std::erase_if(owed, [](const Entry& entry)
                      {
                          return entry.tiles.empty();
                      });
    }

    bool writeLocked(const Entry& entry) const
    {
        for (const auto& [object, open] : locks)
            if (open.write)
                for (const auto& [begin, end] : open.ranges)
                    if (begin < end && overlaps(entry, begin, end))
                        return true;
        return false;
    }

    Entry* live(std::uint64_t mark)
    {
        for (auto& entry : entries)
            for (const auto& tile : entry.tiles)
                if (tile.record.mark == mark)
                    return &entry;
        return nullptr;
    }

    static Tile* tileOf(Entry& entry, std::uint64_t mark) noexcept
    {
        for (auto& tile : entry.tiles)
            if (tile.record.mark == mark)
                return &tile;
        return nullptr;
    }

    // Pending tiles write without commit; ending one ends older committed
    // overlaps with the same ending, counted as overlaps. Exclude handled
    // ranges; the newer resolve ends/supersedes them when it takes effect.
    template <typename Predicate>
    void endWhere(Predicate&& predicate, std::pair<std::uint64_t, std::uint64_t> handled = {}, Ending ending = Ending::Owed)
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pending;
        std::erase_if(entries, [&](Entry& entry)
                      {
                          if (!predicate(std::as_const(entry)))
                              return false;
                          if (!entry.committed)
                              pending.emplace_back(entry.record.copy.destBase, std::uint64_t(entry.record.copy.destBase) + (entry.record.destEnd - entry.record.copy.destBase));
                          noteEnded(entry, ending);
                          retiring.push_back(std::move(entry));
                          return true;
                      });
        if (pending.empty())
            return;
        std::erase_if(entries, [&](Entry& entry)
                      {
                          if (!entry.committed || (handled.first < handled.second && overlaps(entry, handled.first, handled.second)) ||
                              std::none_of(pending.begin(), pending.end(), [&](const auto& range)
                                           {
                                               return overlaps(entry, range.first, range.second);
                                           }))
                              return false;
                          overlapCount.fetch_add(1, std::memory_order_relaxed);
                          noteEnded(entry, ending);
                          retiring.push_back(std::move(entry));
                          return true;
                      });
    }

    // Activate the entry first recorded at mark. Superseded same-range/covered
    // whole-tile entries end with images dropped; other overlaps end with
    // images owed. Covered owed tiles drop too.
    void commit(std::uint64_t mark)
    {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry)
                                        {
                                            return entry.record.mark == mark;
                                        });
        if (found == entries.end())
            return;
        found->committed                 = true;
        const auto                 index = static_cast<std::size_t>(found - entries.begin());
        const auto&                newer = entries[index];
        const auto                 begin = std::uint64_t(newer.record.copy.destBase), end = begin + (newer.record.destEnd - newer.record.copy.destBase);
        std::uint64_t              version = 1;
        std::vector<std::uint64_t> dropped, ended;
        for (const auto& entry : entries)
            if (entry.record.mark != mark && entry.committed && overlaps(entry, begin, end))
            {
                if (sameRange(entry.record, newer.record) && covers(newer, entry))
                {
                    version = std::max(version, entry.version + 1);
                    dropped.push_back(entry.record.mark);
                }
                else if (wholeOver(newer, entry))
                {
                    droppedCovered.fetch_add(1, std::memory_order_relaxed);
                    dropped.push_back(entry.record.mark);
                }
                else
                {
                    overlapCount.fetch_add(1, std::memory_order_relaxed);
                    ended.push_back(entry.record.mark);
                }
            }
        endOwed(
            [&](const Entry& entry)
            {
                return overlaps(entry, begin, end) && ((sameRange(entry.record, newer.record) && covers(newer, entry)) || wholeOver(newer, entry));
            },
            Ending::Dropped);
        for (const auto* marks : { &dropped, &ended })
            endWhere(
                [&](const Entry& entry)
                {
                    return std::find(marks->begin(), marks->end(), entry.record.mark) != marks->end();
                },
                {},
                marks == &ended ? Ending::Owed : Ending::Dropped);
        live(mark)->version = version;
    }
};

// The registry the guest resource hooks feed (guestResourceObservers), off
// until one is published.
namespace detail
{

inline std::mutex& ResolveSurfacesMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::weak_ptr<ResolveSurfaceRegistry>& PublishedResolveSurfaceSlot()
{
    static std::weak_ptr<ResolveSurfaceRegistry> slot;
    return slot;
}

} // namespace detail

// Publishes `surfaces` to the hooks for as long as it lives; null withdraws it.
inline void PublishResolveSurfaces(const std::shared_ptr<ResolveSurfaceRegistry>& surfaces)
{
    std::lock_guard lock(detail::ResolveSurfacesMutex());
    detail::PublishedResolveSurfaceSlot() = surfaces;
    SetGuestResourceObserver(kGuestResolveSurfaces, surfaces != nullptr);
}

inline std::shared_ptr<ResolveSurfaceRegistry> PublishedResolveSurfaces()
{
    std::lock_guard lock(detail::ResolveSurfacesMutex());
    auto            surfaces = detail::PublishedResolveSurfaceSlot().lock();
    if (!surfaces)
        SetGuestResourceObserver(kGuestResolveSurfaces, false);
    return surfaces;
}

} // namespace rerevved::gpu::guest_d3d
