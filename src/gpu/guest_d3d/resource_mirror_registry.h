#pragma once

#include "gpu/guest_d3d/guest_resource_read.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Guest physical storage a resource names: [address, address + size).
struct GuestResourceRange
{
    std::uint32_t address = 0, size = 0;
};

// A resource as its create hook found it after Original returned. `words`
// are in the form a draw reads them (see GuestResourceRead), so a bind
// compares them directly. A texture has a base and a mip range, a buffer one.
struct GuestResourceRegistration
{
    GuestResourceKind                 kind   = GuestResourceKind::Texture;
    std::uint32_t                     header = 0;
    std::array<std::uint32_t, 6>      words{};
    std::array<GuestResourceRange, 2> ranges{};
};

// Guest D3D object header sizes the create hooks read.
inline constexpr std::size_t kGuestTextureHeaderBytes = 0x34;
inline constexpr std::size_t kGuestBufferHeaderBytes  = 0x20;
inline constexpr std::size_t kGuestSurfaceHeaderBytes = 0x30;

// The guest's physical alias of a CPU address, as SetTexture, SetStreamSource
// and the index buffer draw convert one (0x8269AF10, 0x8268FB08).
constexpr std::uint32_t GuestPhysicalAddress(std::uint32_t address) noexcept
{
    return (address & 0x1FFFFFFFU) + (((address >> 20) + 512) & 0x1000U);
}

// The header bytes a kind's create hooks read.
std::size_t GuestResourceHeaderBytes(GuestResourceKind kind) noexcept;
// The words a draw binds from a header's bytes (see DescribeGuestResource),
// without its ranges; false for a header too short for its kind.
bool GuestResourceWords(GuestResourceKind kind, std::span<const std::uint8_t> bytes, std::array<std::uint32_t, 6>& words) noexcept;

// Decode big-endian kind-sized header; short/surface(no own storage) returns
// false. Texture+0x1C..+0x30 merges as SetTexture; ranges prefer full fetch
// layout, then base-only, else none.
bool DescribeGuestResource(GuestResourceKind kind, std::uint32_t header, std::span<const std::uint8_t> bytes, GuestResourceRegistration& registration);

// Whether a draw's texture fetch names the same storage as a registered
// texture's words: type, pitch, tiling, format, endian, stacking, base, size,
// dimension, packed mips and mip base. Sampler and clamp bits are ignored.
bool GuestTextureStorageMatches(const std::array<std::uint32_t, 6>& fetch, const std::array<std::uint32_t, 6>& registered) noexcept;

// Whether a registered object's words name the storage `words` do: the
// texture storage bits, or every buffer word.
bool GuestResourceWordsMatch(GuestResourceKind kind, const std::array<std::uint32_t, 6>& words, const std::array<std::uint32_t, 6>& registered) noexcept;

// TakeCounts separates dirty notices, first binds, evictions, and unsized frees
// from captured ranges.
struct GuestResourceMirrorCounts
{
    std::uint64_t                                      hits = 0, fallbacks = 0, resolved = 0, uploads = 0, uploadBytes = 0;
    std::uint64_t                                      watchDirty = 0, bindRegistrations = 0, viewEvictions = 0, freeUnsized = 0;
    std::array<std::uint64_t, kGuestResourceKindCount> locks{};
};

std::string FormatGuestResourceMirrorCounts(const GuestResourceMirrorCounts& counts);

// Mirrors key(header, serial), start dirty. Dirty bind watches/reads whole
// range once, then shares immutable bytes until lock/unlock/write/resolve
// dirties it. Locked never captures; bind validates words or falls back.
class GuestResourceMirrors
{
public:
    struct Config
    {
        // Arms the guest write notice over physical [address, address + size)
        // and reports whether all of it is watched now. Unset, or false for a
        // range, binds of it fall back.
        std::function<bool(std::uint32_t, std::uint32_t)> watch;
        // Bytes the registry's copies may hold; a capture beyond it falls back.
        std::size_t budget = std::size_t(1) << 30;
    };

    // Reads guest physical [address, address + size) through the bytes path.
    using RangeRead = std::function<GuestBytes(std::uint32_t, std::uint32_t)>;

    explicit GuestResourceMirrors(Config config);
    ~GuestResourceMirrors();
    GuestResourceMirrors(const GuestResourceMirrors&)            = delete;
    GuestResourceMirrors& operator=(const GuestResourceMirrors&) = delete;

    // Returns the object's serial, never zero. `bound` counts a registration
    // made at a bind.
    std::uint64_t Register(const GuestResourceRegistration& registration, bool bound = false);
    // Whether `header` is a live object of `kind` whose words match.
    bool Holds(GuestResourceKind kind, std::uint32_t header, const std::array<std::uint32_t, 6>& words) const;
    // The lock core opened (0x82697CA0) or closed (0x82697F38) a lock on the
    // object; `kind` is the locked object's, before a level names its texture.
    void Locked(GuestResourceKind kind, std::uint32_t header) noexcept;
    void Unlocked(std::uint32_t header) noexcept;
    void Released(std::uint32_t header) noexcept;
    // The device was released; every object is released with it.
    void ReleasedAll() noexcept;
    // A physical free releases intersecting objects. With size 0, extent is its
    // page or the largest longer registered range starting within that page;
    // count this unsized-free fallback.
    void Freed(std::uint32_t physical, std::uint32_t size = 0) noexcept;
    // Physical [begin, end) changed or may change. With `resolve`, a resolve
    // image writes it: the range stays on the bytes path from then on.
    void Written(std::uint64_t begin, std::uint64_t end, bool resolve = false) noexcept;
    // The SDK's physical memory invalidation callback, with the registry as
    // its context; permits unwatching only the pages it names.
    static std::pair<std::uint32_t, std::uint32_t> WriteNotice(void* context, std::uint32_t physical, std::uint32_t length, bool exact) noexcept;

    // The mirrored bytes for a draw's read, or nothing for the caller's
    // bytes path. Takes no lock while it calls `watch` or `read`.
    std::optional<GuestBytes> Bind(const GuestResourceRead& request, const RangeRead& read);

    GuestResourceMirrorCounts TakeCounts() noexcept;
    // Live registered objects, all entries including released ones not yet
    // erased, and the bytes their copies hold.
    std::size_t                  Objects() const;
    std::size_t                  Entries() const;
    std::size_t                  HeldBytes() const;
    std::optional<std::uint64_t> SerialOf(std::uint32_t header) const;

    // Keeps a handle until destruction, which drops it before anything else,
    // such as the write notice registration that names this registry.
    void Hold(std::shared_ptr<void> handle);

private:
    // Sub-range copies an object keeps.
    static constexpr std::size_t kMaxObjectViews = 8;

    using Retired = std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>; // freed after the lock

    struct View
    {
        std::uint32_t                                    offset = 0, size = 0;
        std::uint64_t                                    order = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    };

    struct Range
    {
        GuestResourceRange                               range;
        std::uint64_t                                    generation = 1, captured = 0;
        bool                                             resolved = false;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        // Full/sub-range copies share held-byte budget; each object keeps at
        // most kMaxObjectViews sub-range copies. Copy lifetime is independent
        // of registry release while frames share it.
        std::vector<View> views; // sub-range copies of bytes
    };

    struct Object
    {
        GuestResourceRegistration registration;
        std::array<Range, 2>      ranges;
        std::uint32_t             locks     = 0;
        std::uint64_t             viewOrder = 0;
        bool                      released  = false;
    };

    struct Found
    {
        std::uint64_t serial = 0;
        std::size_t   range  = 0;
    };

    // What find reads of a bind, for the lookup memo.
    struct FindKey
    {
        GuestResourceKind            kind    = GuestResourceKind::Texture;
        std::uint32_t                address = 0, size = 0, header = 0;
        std::array<std::uint32_t, 6> words{};
        bool                         operator==(const FindKey&) const = default;
    };

    struct FindKeyHash
    {
        std::size_t operator()(const FindKey& key) const noexcept;
    };

    std::optional<Found> find(const GuestResourceRead& request) const;
    // find through the request's hint, which it reuses or rewrites.
    std::optional<Found> hinted(const GuestResourceRead& request) const;
    std::optional<Found> scan(const GuestResourceRead& request) const;
    void                 drop(Range& range, Retired& retired) noexcept;
    void                 erase(std::uint64_t serial, Retired& retired);
    void                 release(std::uint64_t serial, Object& object);
    void                 drain(Retired& retired);
    void                 dirty(Object& object) noexcept;
    void                 written(std::uint64_t begin, std::uint64_t end, bool resolve, bool notice) noexcept;
    GuestBytes           view(Object& object, Range& range, const GuestResourceRead& request, Retired& retired);

    Config                                                      config;
    mutable std::mutex                                          mutex;
    std::map<std::uint64_t, Object>                             objects;
    std::unordered_map<std::uint32_t, std::uint64_t>            headers;
    std::multimap<std::uint32_t, std::pair<std::uint64_t, int>> addresses; // range start -> (serial, range)
    std::uint32_t                                               longest = 0;
    std::uint64_t                                               serials = 0;
    std::size_t                                                 held    = 0;
    // Released object stops binding at once; queue erases next
    // registration/bind without scanning. In-flight shared buffers live through
    // frame fence retirement. Re-registering a header replaces object.
    std::vector<std::uint64_t> releasedQueue; // serials to erase
    // find results by bind. An object's registration never changes, so a
    // result holds until a registration may add a newer match (Register clears
    // the memo) or its object is released (checked at each hit).
    static constexpr std::size_t                                           kMaxMemo = 8192;
    mutable std::unordered_map<FindKey, std::optional<Found>, FindKeyHash> memo;
    // Advances whenever the memo is cleared for validity, so a bind hint of
    // an earlier generation is looked up again. A hint names its registry by
    // `token`, unique for the process, since a reader's hints outlive one.
    std::uint64_t       findGeneration = 1;
    const std::uint64_t token;

    // Locks no live object holds: those opened before a registration, and
    // `inherited` ones a released object still held over `ranges`.
    struct PendingLock
    {
        std::uint32_t                     locks = 0, inherited = 0;
        std::array<GuestResourceRange, 2> ranges{};
    };

    // Locks predating registration or inherited from released predecessor
    // survive by header until free. If lock bookkeeping fails, degraded makes
    // every bind fall back until device release.
    std::unordered_map<std::uint32_t, PendingLock> pendingLocks;     // by header
    bool                                           degraded = false; // a lock went unrecorded
    std::shared_ptr<void>                          handle;

    std::atomic<std::uint64_t>                                      hits{ 0 }, fallbacks{ 0 }, resolvedReads{ 0 }, uploads{ 0 }, uploadBytes{ 0 };
    std::atomic<std::uint64_t>                                      watchDirty{ 0 }, bindRegistrations{ 0 }, viewEvictions{ 0 }, freeUnsized{ 0 };
    std::array<std::atomic<std::uint64_t>, kGuestResourceKindCount> lockCounts{};
};

// The registry the create, unlock and release hooks feed. Off until a
// registry is published; a disabled hook pays one relaxed load.
extern std::atomic<bool> guestResourceMirrorsEnabled;
// Publishes `mirrors` to the hooks for as long as it lives; null withdraws it.
void                                  PublishGuestResourceMirrors(const std::shared_ptr<GuestResourceMirrors>& mirrors);
std::shared_ptr<GuestResourceMirrors> PublishedGuestResourceMirrors();

} // namespace rerevved::gpu::guest_d3d
