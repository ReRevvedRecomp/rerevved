#pragma once

#include "native_draw_replay.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace rerevved::gpu
{

class NativeRenderFrameTextureVersion;

namespace detail
{

inline std::uint64_t OwnedAdd(std::uint64_t left, std::uint64_t right)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        throw std::bad_alloc{};
    return left + right;
}

inline std::uint64_t OwnedMultiply(std::uint64_t left, std::uint64_t right)
{
    if (right && left > std::numeric_limits<std::uint64_t>::max() / right)
        throw std::bad_alloc{};
    return left * right;
}

// The renderer serializes limit changes with its admission/worker ownership.
// This object outlives every allocator referring to it, including moved snapshots.
struct OwnedHeapBudget
{
    std::atomic<std::uint64_t> live{ 0 };
    std::uint64_t              limit = 0;

    void Claim(std::uint64_t bytes)
    {
#if !defined(_WIN32) || !defined(__clang__) || __clang_major__ != 22 || __clang_minor__ != 1 || __clang_patchlevel__ != 4 || !defined(_MSVC_STL_VERSION) || _MSVC_STL_VERSION != 143 || _MSVC_STL_UPDATE != 202503L || _ITERATOR_DEBUG_LEVEL != 0 || !defined(NDEBUG)
        throw std::bad_alloc{};
#else
        static_assert(sizeof(void*) == 8);
        auto current = live.load(std::memory_order_relaxed);
        do
        {
            if (current > limit || bytes > limit - current)
                throw std::bad_alloc{};
        } while (!live.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
#endif
    }

    void Release(std::uint64_t bytes) noexcept
    {
        live.fetch_sub(bytes, std::memory_order_relaxed);
    }
};

template <class T>
struct OwnedAllocator
{
    using value_type                             = T;
    using propagate_on_container_move_assignment = std::true_type;
    OwnedHeapBudget* budget                      = nullptr;

    OwnedAllocator() = default;

    explicit OwnedAllocator(OwnedHeapBudget* value)
    : budget(value)
    {
    }

    template <class U>
    OwnedAllocator(const OwnedAllocator<U>& other)
    : budget(other.budget)
    {
    }

    T* allocate(std::size_t count)
    {
        const auto bytes = OwnedMultiply(count, sizeof(T));
        if (budget)
            budget->Claim(bytes);
        try
        {
            return std::allocator<T>{}.allocate(count);
        }
        catch (...)
        {
            if (budget)
                budget->Release(bytes);
            throw;
        }
    }

    void deallocate(T* values, std::size_t count) noexcept
    {
        std::allocator<T>{}.deallocate(values, count);
        if (budget)
            budget->Release(count * sizeof(T));
    }

    template <class U>
    bool operator==(const OwnedAllocator<U>& other) const noexcept
    {
        return budget == other.budget;
    }
};

template <class T>
using OwnedSet = std::unordered_set<T, std::hash<T>, std::equal_to<T>, OwnedAllocator<T>>;
using OwnerSet = OwnedSet<const void*>;

inline std::uint64_t OwnerSetBytes(std::uint64_t count, std::uint64_t buckets, std::uint64_t node = 24)
{
    return OwnedAdd(OwnedMultiply(16, buckets), OwnedMultiply(node, OwnedAdd(count, 1)));
}

struct OwnerSetForecast
{
    std::uint64_t bytes, peak;
};

inline OwnerSetForecast ForecastOwnerSet(std::uint64_t count, std::uint64_t buckets, std::uint64_t additions, std::uint64_t node = 24)
{
    const auto end  = OwnedAdd(count, additions);
    auto       peak = OwnerSetBytes(count, buckets, node);
    while (end > buckets)
    {
        const auto next = OwnedMultiply(buckets, buckets < 512 ? 8 : 2);
        peak            = std::max(peak, OwnedAdd(OwnedMultiply(16, OwnedAdd(buckets, next)), OwnedMultiply(node, OwnedAdd(buckets, 2))));
        buckets         = next;
    }
    const auto bytes = OwnerSetBytes(end, buckets, node);
    return { bytes, std::max(peak, bytes) };
}

inline OwnerSetForecast ForecastOwnerSet(std::uint64_t count)
{
    return ForecastOwnerSet(0, 8, count);
}

// A stack callback keeps the allocation-free census and the tracked walk on the
// same ownership policy. Null identities are non-shareable scalar charges.
struct OwnedVisitor
{
    void* context;
    void (*emit)(void*, const void*, std::uint64_t);

    template <class F>
    static OwnedVisitor From(F& function)
    {
        return { &function, [](void* value, const void* owner, std::uint64_t bytes)
                 {
                     (*static_cast<F*>(value))(owner, bytes);
                 } };
    }

    void Add(const void* owner, std::uint64_t bytes) const
    {
        emit(context, owner, bytes);
    }
};

struct OwnedCollector
{
    OwnerSet&       owners;
    const OwnerSet* borrowed = nullptr;
    std::uint64_t   ceiling  = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t   bytes    = 0;

    void operator()(const void* owner, std::uint64_t value)
    {
        if (owner)
        {
            if ((borrowed && borrowed->contains(owner)) || owners.contains(owner))
                return;
            if (owners.size() >= ceiling)
                throw std::bad_alloc{};
            owners.insert(owner);
        }
        bytes = OwnedAdd(bytes, value);
    }

    OwnedVisitor Visitor()
    {
        return OwnedVisitor::From(*this);
    }
};

template <class Walk>
std::pair<std::uint64_t, std::uint64_t> CensusOwnedBytes(const Walk& walk)
{
    std::uint64_t occurrences = 0, bytes = 0, unique = 0;
    auto          inventory = [&](const void* owner, std::uint64_t value)
    {
        if (owner)
            occurrences = OwnedAdd(occurrences, 1);
        else
            bytes = OwnedAdd(bytes, value);
    };
    walk(OwnedVisitor::From(inventory));
    for (std::uint64_t index = 0; index < occurrences; ++index)
    {
        const void*   selected = nullptr;
        std::uint64_t ordinal = 0, contribution = 0;
        auto          select = [&](const void* owner, std::uint64_t value)
        {
            if (owner && ordinal++ == index)
            {
                selected     = owner;
                contribution = value;
            }
        };
        walk(OwnedVisitor::From(select));
        ordinal       = 0;
        bool seen     = false;
        auto previous = [&](const void* owner, std::uint64_t)
        {
            if (owner)
            {
                if (ordinal < index && owner == selected)
                    seen = true;
                ++ordinal;
            }
        };
        walk(OwnedVisitor::From(previous));
        if (!seen)
        {
            bytes  = OwnedAdd(bytes, contribution);
            unique = OwnedAdd(unique, 1);
        }
    }
    return { bytes, unique };
}

} // namespace detail

// Guest EDRAM word layouts a resolve packs into an R32Uint destination.
enum class NativeGuestWordPacking : std::uint8_t
{
    None,
    // R | G << 8 | B << 16 | A << 24, 8-bit unorm.
    Color8888,
    // R | G << 10 | B << 20 | A << 30, 10:10:10:2 unorm.
    Color2101010,
    // depth24 << 8 | stencil.
    Depth24Stencil8,
    // half(R) | half(G) << 16 from a k_16_16 fixed-point slot (Rg16Snorm).
    Float16x2,
};

struct NativeRenderFrameTexture
{
    std::uint64_t                 id     = 0;
    std::uint32_t                 width  = 0;
    std::uint32_t                 height = 0;
    NativeDrawReplayTextureFormat format = NativeDrawReplayTextureFormat::Rgba8;
    // Imports may name a completed registry entry or an exact submitted
    // producer proof with matching layout on this renderer's native queue.
    std::uint64_t                                          persistentKey = 0;
    bool                                                   imported      = false;
    std::shared_ptr<const NativeRenderFrameTextureVersion> producerVersion;
    // An imported R32Uint texture's guest-word packing, which gives draws the
    // typed view a resolve in the same frame would.
    NativeGuestWordPacking importedWords = NativeGuestWordPacking::None;

    friend bool operator==(const NativeRenderFrameTexture& left, const NativeRenderFrameTexture& right)
    {
        return left.id == right.id && left.width == right.width && left.height == right.height && left.format == right.format &&
               left.persistentKey == right.persistentKey && left.imported == right.imported && left.importedWords == right.importedWords && left.producerVersion == right.producerVersion &&
               !left.producerVersion.owner_before(right.producerVersion) && !right.producerVersion.owner_before(left.producerVersion);
    }
};

struct NativeRenderFrameResolve
{
    std::uint64_t textureId    = 0;
    bool          depth        = false;
    std::uint32_t colorTarget  = 0;
    std::uint32_t sourceX      = 0;
    std::uint32_t sourceY      = 0;
    std::uint32_t width        = 0;
    std::uint32_t height       = 0;
    std::uint32_t destinationX = 0;
    // The exercised resolves write full-height strips into a texture.
    std::array<std::uint32_t, 2> samples     = { 0, 0 };
    bool                         swapRedBlue = false;
    float                        valueScale  = 1.0F;
    NativeGuestWordPacking       guestWords  = NativeGuestWordPacking::None;
    // Copies the destination to host memory after the pass. A recorded prefix
    // returns it in NativeDrawReplayResult::rawReadbacks, readable at its fence.
    bool readback = false;
};

// A target without a depth surface identity.
inline constexpr std::uint32_t kNoDepthEdramBase = 0xFFFFFFFFU;

struct NativeRenderFramePass
{
    // Attachment preservation is explicit across adjacent passes. Guest EDRAM
    // aliases must be normalized by the title before entering this interface.
    bool                                  preservePreviousAttachments = false;
    std::vector<NativeDrawReplayRecipe>   draws;
    std::vector<NativeRenderFrameResolve> resolves;

    bool                          ValidateDrawStorage(std::string& error) const;
    std::size_t                   DrawCount() const;
    const NativeDrawReplayRecipe& Draw(std::size_t index) const;

    struct Target
    {
        std::uint32_t                width       = 0;
        std::uint32_t                height      = 0;
        std::uint32_t                sampleCount = 0;
        NativeDrawReplayTargetFormat format      = NativeDrawReplayTargetFormat::Rgba8;
        std::uint32_t                edramBase   = 0;
        // EDRAM base of the depth surface. Part of the identity, so a preserve
        // keeps the same depth surface as well as the same color target.
        std::uint32_t depthEdramBase = kNoDepthEdramBase;
        // RT1.. of an MRT pass; entries beyond the count stay zero. Without
        // extras the four host slots share the RT0 format.
        std::uint8_t                                extraColorTargets = 0;
        std::array<NativeDrawReplayTargetFormat, 3> extraFormats{};
        // Identity only, like edramBase.
        std::array<std::uint32_t, 3> extraEdramBases{};
        friend bool                  operator==(const Target&, const Target&) = default;
    };

    struct Clear
    {
        NativeDrawReplayScissor rectangle;
        bool                    color        = false;
        bool                    depth        = false;
        bool                    stencil      = false;
        std::uint8_t            colorTargets = 1;
        std::array<float, 4>    colorValue{};
        float                   depthValue   = 1.0F;
        std::uint8_t            stencilValue = 0;
    };

    // Clears execute before this pass's draws. An explicit target also permits
    // clear-only passes between draws and resolves without invented geometry.
    Target             target;
    std::vector<Clear> clears;
    // One-based earlier pass whose single-sample 32-bit color bytes initialize
    // this target before its clears/draws, including an EDRAM format alias.
    std::uint32_t                                                 colorSourcePass = 0;
    std::vector<std::shared_ptr<const NativeDrawReplayAdmission>> admittedDraws;
    // Work on the swap's presented target, which may later render at a host
    // extent. Guest-memory resolve frames never set it and stay at guest scale.
    bool presented = false;
    // With identityAttachments: continue the attachments the latest presented
    // pass of this chain left, across the off-screen passes since, which keep
    // their own attachments. The target must equal that pass's target.
    bool resumeAttachments = false;
};

NativeRenderFramePass::Target GetNativeRenderFrameTarget(const NativeRenderFramePass& pass);
// The attachments a draw writes, without a pass identity.
NativeRenderFramePass::Target NativeRenderFrameDrawTarget(const NativeDrawReplayRecipe& draw);
// The format of host color slot 0..3; unused slots of an MRT pass use RT0's.
NativeDrawReplayTargetFormat NativeRenderFrameSlotFormat(const NativeRenderFramePass::Target& target, std::uint32_t slot);

struct NativeRenderExtent
{
    std::uint32_t width = 0, height = 0;
    friend bool   operator==(const NativeRenderExtent&, const NativeRenderExtent&) = default;
};

// After frame recording, the store key retains the produced texture for later
// imports.
struct NativeRenderFrameSurface
{
    std::uint64_t textureId = 0, key = 0, depthKey = 0;
    // width/height assemble tile resolves at(x, y) in key's produced-format
    // texture. depthKey copies matching Depth24Stencil8 words as R32Float using
    // bytes-path k_24_8 decode.
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    friend bool   operator==(const NativeRenderFrameSurface&, const NativeRenderFrameSurface&) = default;
};

struct NativeRenderFrame
{
    // Textures have no CPU payload. Resolves initialize them in order, and a
    // draw may sample one only after its complete extent has been written.
    std::vector<NativeRenderFrameTexture> textures;
    std::vector<NativeRenderFramePass>    passes;
    std::uint64_t                         outputTextureId = 0;
    std::array<std::uint8_t, 4>           outputSwizzle   = { 0, 1, 2, 3 };
    std::array<std::uint32_t, 256>        gammaTable{};
    std::filesystem::path                 outputPath;
    bool                                  returnResolvedOutput = false;
    bool                                  returnGpuOutput      = false;
    bool                                  retainOnly           = false;
    bool                                  returnRawOutput      = false;
    // Presented and off-screen passes keep separate attachments, so a
    // presented pass may resume after off-screen work. Every packet of a
    // chain and its terminal graph agree on it.
    bool identityAttachments = false;
    // Draws may bind Color8888 or Color2101010 guest words as Rgba8 or
    // Rgb10a2; a renderer without castable storage refuses the frame.
    bool typedGuestWords = false;
    // Zero renders presented passes at the guest extent.
    NativeRenderExtent presentExtent;
    // Resolve surfaces: each produced texture enters the renderer's surface
    // store once this frame recorded, replacing the key's previous texture,
    // and each released key leaves it then. Store textures retire by fence.
    std::vector<NativeRenderFrameSurface> surfaces;
    std::vector<std::uint64_t>            releasedSurfaces;
    // continuedPrefixPasses joins appended prefix passes to a terminal; zero
    // means standalone or already-open full graph.
    std::size_t continuedPrefixPasses = 0;
};

bool ValidateNativeRenderFrame(const NativeRenderFrame& frame, std::string& error);
// One compatible preserved chain may produce partial strips before the
// terminal graph proves complete resources and admits publication or output.
bool ValidateNativeRenderFramePrefix(const NativeRenderFrame& frame, std::string& error);

// Validates new packets across fresh target changes. Preservation
// requires the exact preceding target; descriptors and resolve extents persist.
// A rejected append ends this state; only the complete graph admits output.
class NativeRenderFramePrefixState
{
public:
    NativeRenderFramePrefixState();
    ~NativeRenderFramePrefixState();
    NativeRenderFramePrefixState(NativeRenderFramePrefixState&&) noexcept;
    NativeRenderFramePrefixState& operator=(NativeRenderFramePrefixState&&) noexcept;
    bool                          Append(const NativeRenderFrame& packet, std::string& error);
    // Validates the terminal frame the appended prefix will run inside, so its
    // draws may sample the prefix's resolves; the chain itself is unchanged.
    bool ValidateTerminal(const NativeRenderFrame& frame, std::string& error) const;
    // Validates a tail terminal whose continuedPrefixPasses names exactly the
    // appended passes, as ValidateTerminal does but in place: the chain ends
    // either way.
    bool FinishTerminal(const NativeRenderFrame& tail, std::string& error);

private:
    friend bool ValidateNativeRenderFrame(const NativeRenderFrame&, std::string&);
    friend class NativeRendererD3D12;
    explicit NativeRenderFramePrefixState(detail::OwnedHeapBudget* budget);
    static std::uint64_t OwnedMetadataBytes();
    void                 VisitOwnedBytes(detail::OwnedVisitor visitor) const;
    void                 ForecastOwnedHeap(const NativeRenderFrame& frame, std::uint64_t& growth, std::uint64_t& peak) const;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Prefix passes precede tail; shift tail aliases, append undeclared prefix
// textures in order, combine typed flags, clear continuedPrefixPasses. Count
// mismatch fails unchanged.
bool JoinNativeRenderFrameTail(NativeRenderFrame& tail, std::vector<NativeRenderFramePass>&& prefixPasses, std::vector<NativeRenderFrameTexture> prefixTextures, bool prefixTypedGuestWords, std::string& error);
// Splitting removes recorded prefix passes while preserving declarations; alias
// or alias-source drops fail without mutation.
bool SplitNativeRenderFrameTail(NativeRenderFrame& frame, std::size_t prefixPasses, std::string& error);

inline constexpr std::size_t NativeRenderBatchCommands = 4096;
// Divide oversized passes without creating a presentation boundary. Aliases
// reference the completed final batch of their original source pass.
bool BatchNativeRenderFrame(NativeRenderFrame& frame, std::string& error);

namespace detail
{

void VisitNativeDrawOwnedBytes(const NativeDrawReplayRecipe& draw, OwnedVisitor visitor, bool local = true);
void VisitNativeRenderFrameOwnedBytes(const NativeRenderFrame& frame, OwnedVisitor visitor);
// The passes alone, as VisitNativeRenderFrameOwnedBytes counts a frame's.
void VisitNativeRenderFramePassesOwnedBytes(const std::vector<NativeRenderFramePass>& passes, OwnedVisitor visitor);

} // namespace detail

} // namespace rerevved::gpu
