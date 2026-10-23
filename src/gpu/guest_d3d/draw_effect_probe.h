#pragma once

#include "indexed_buffer_gate.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// 0x826A39F8 whole-device diff runs off by default and records admitted
// IndexedBuffer changes without altering Original. Built only with
// REREVVED_DEVELOPER.

// Bytes of G that matter for a draw's effect. The ring buffer is not inside G.
inline constexpr std::uint32_t kDrawEffectGlobalSize = 0x5B10;
// One root in this many is sampled on average, and sampling stops after the limit.
inline constexpr std::uint32_t kDrawEffectSampleInterval = 16;
inline constexpr std::uint64_t kDrawEffectSampleLimit    = 100000;
// At most this many sampled roots in one guest swap, so samples spread over
// the session instead of spending the total cap on its first seconds.
inline constexpr std::uint32_t kDrawEffectSamplesPerSwap = 8;

using DrawEffectWordReader = IndexedBufferWordReader;
// Copies guest bytes as stored (big-endian); false when any is unreadable.
using DrawEffectBlockReader = bool (*)(const std::uint8_t* base, std::uint32_t address, std::span<std::uint8_t> bytes);

struct DrawEffectReaders
{
    DrawEffectWordReader  word  = nullptr;
    DrawEffectBlockReader block = nullptr; // null reads word by word through `word`
};

// Copies [graphics, graphics + out.size()); false when any byte is unreadable.
bool ReadDrawEffectGlobal(const DrawEffectReaders& readers, const std::uint8_t* base, std::uint32_t graphics, std::span<std::uint8_t> out) noexcept;

enum class DrawEffectClass : std::uint8_t
{
    ExpectedDirty,  // dirty qwords G+0x00..0x27, zeroed by the flush copies
    ExpectedCursor, // ring cursor G+0x30
    ExpectedCache,  // render-register caches written by 0x826ADD88
    // Tiled samples only:
    ExpectedList,      // tile record list header G+0x33C0..0x33CF
    ExpectedAllocator, // side buffer allocator words written by the list growth
    Unexpected,
};
inline constexpr std::size_t kDrawEffectClassCount = static_cast<std::size_t>(DrawEffectClass::Unexpected) + 1;

const char* DrawEffectClassName(DrawEffectClass kind) noexcept;
// Classifies a byte offset in G against the statically derived effect set of
// an untiled root.
DrawEffectClass ClassifyDrawEffectOffset(std::uint32_t offset) noexcept;
// The same for a TiledRecord root, which adds the list header and the growth
// allocator words.
DrawEffectClass ClassifyTiledDrawEffectOffset(std::uint32_t offset) noexcept;

// Chooses whether the root with this index is sampled; `interval` is the mean
// spacing. Tests install a deterministic one.
using DrawEffectSelector = bool (*)(std::uint64_t index, std::uint32_t interval);
// Hashes the index, so a stable per-frame draw count cannot alias with the
// interval and leave call positions unsampled.
bool DrawEffectSelectRandom(std::uint64_t index, std::uint32_t interval) noexcept;

// True once this thread has allocated its sample buffers, which it does on its
// first sampled root only.
bool DrawEffectThreadBuffersAllocated() noexcept;

// Sentinel for a swap sequence that never happened.
inline constexpr std::uint64_t kDrawEffectNoSwap = ~0ULL;

// Points inside the probe where a test can interleave another actor.
enum class DrawEffectHookPoint : std::uint8_t
{
    BeginAfterMarker, // BeginRoot captured the root-start count, before it reads the in-flight count
    Reserved,         // BeginRoot reserved a sample; `value` is the swap sequence it recorded
    EndAfterInFlight, // EndRoot read the in-flight count, before it compares the root-start count
    EndBeforeCommit,  // EndRoot holds a finished diff, before it commits it
};

namespace detail
{

// Test hook; null in production. Set before any probed thread runs.
extern void (*drawEffectHook)(DrawEffectHookPoint point, std::uint64_t value);

} // namespace detail

struct DrawEffectConfig
{
    std::uint32_t      interval = kDrawEffectSampleInterval;
    std::uint64_t      limit    = kDrawEffectSampleLimit;
    std::uint32_t      perSwap  = kDrawEffectSamplesPerSwap; // 0: no per-swap budget
    DrawEffectSelector selector = nullptr;                   // null: DrawEffectSelectRandom
};

struct DrawEffectCounts
{
    std::uint64_t roots                = 0; // IndexedBuffer roots seen
    std::uint64_t selected             = 0; // roots chosen by the sampler
    std::uint64_t gateSkipped          = 0; // selected, a tiling byte other than the tiled-record one
    std::uint64_t gateUnreadable       = 0; // selected, tiling word unreadable
    std::uint64_t snapshotUnreadable   = 0; // before or after copy of G unreadable
    std::uint64_t sampled              = 0; // before and after both diffed
    std::uint64_t sampledIsolated      = 0; // sampled with no other draw root active
    std::uint64_t sampledConcurrent    = 0; // sampled while another draw root was active
    std::uint64_t sampledWithChange    = 0; // sampled, at least one byte changed
    std::uint64_t abandoned            = 0; // armed, but the root never completed
    std::uint64_t limited              = 0; // selected after the total cap was reached
    std::uint64_t budgetSkipped        = 0; // admitted after the swap's budget was spent
    std::uint64_t budgetSwaps          = 0; // swaps whose budget was spent
    std::uint64_t swapsSeen            = 0; // swap boundaries noted
    std::uint64_t firstSampleSwap      = kDrawEffectNoSwap;
    std::uint64_t lastSampleSwap       = kDrawEffectNoSwap;
    std::uint64_t limitHitSwap         = kDrawEffectNoSwap; // swap in which the total cap first refused a root
    std::uint64_t changedOffsets       = 0;                 // distinct offsets that ever changed
    std::uint64_t isolatedOffsets      = 0;                 // of those, changed in an isolated sample
    std::uint64_t unexpectedOffsets    = 0;                 // changed in an isolated sample, outside the expected set
    std::uint64_t unexpectedConcurrent = 0;                 // changed in a concurrent sample, outside the expected set

    // TiledRecord roots, tallied apart from the untiled fields above. They share
    // the sample cap, the swap budget and the unreadable and abandoned counts.
    std::uint64_t tiledSelected             = 0; // selected, tiling byte exactly the tiled-record one
    std::uint64_t tiledSampled              = 0;
    std::uint64_t tiledSampledIsolated      = 0;
    std::uint64_t tiledSampledConcurrent    = 0;
    std::uint64_t tiledSampledWithChange    = 0;
    std::uint64_t tiledChangedOffsets       = 0;
    std::uint64_t tiledIsolatedOffsets      = 0;
    std::uint64_t tiledUnexpectedOffsets    = 0;
    std::uint64_t tiledUnexpectedConcurrent = 0;
    // Distinct offsets that changed in a tiled sample, per DrawEffectClass.
    std::array<std::uint64_t, kDrawEffectClassCount> tiledClassOffsets{};
};

struct DrawEffectOffset
{
    std::uint32_t offset = 0;
    // Samples of each kind in which this byte changed, and the first change of each kind.
    std::uint64_t   isolated = 0, concurrent = 0;
    std::uint8_t    isolatedBefore = 0, isolatedAfter = 0;
    std::uint8_t    concurrentBefore = 0, concurrentAfter = 0;
    DrawEffectClass kind = DrawEffectClass::Unexpected;
};

struct DrawEffectSnapshot
{
    DrawEffectCounts              counts;       // offset totals derive from `offsets` and `tiledOffsets`
    std::vector<DrawEffectOffset> offsets;      // untiled samples, classified by ClassifyDrawEffectOffset
    std::vector<DrawEffectOffset> tiledOffsets; // TiledRecord samples, classified by ClassifyTiledDrawEffectOffset
};

class DrawEffectProbe
{
public:
    explicit DrawEffectProbe(DrawEffectConfig config = {});
    ~DrawEffectProbe();
    DrawEffectProbe(const DrawEffectProbe&)            = delete;
    DrawEffectProbe& operator=(const DrawEffectProbe&) = delete;

    // Called for every draw root of any hook at entry; a sample is concurrent
    // when this count moves while it is open.
    void NoteRoot() noexcept
    {
        rootStarts_.fetch_add(1, std::memory_order_relaxed);
    }

    // Called at each guest swap boundary: resets the per-swap budget and fits
    // the sampling interval to the previous swap's root count.
    void NoteSwap(std::uint64_t sequence) noexcept;

    // BeginRoot runs before Original: inFlight includes this root and is read
    // after root-start count, so later starts move one. True transfers this
    // thread's sample to EndRoot/AbandonRoot for finishing.
    bool BeginRoot(const DrawEffectReaders& readers, const std::uint8_t* base, std::uint32_t graphics, const std::atomic<std::uint64_t>& inFlight) noexcept;
    // After Original including nested draws, EndRoot reads inFlight then start
    // count to see every overlap. Entry r3 fixes G, matching BeginRoot address.
    // Counters/offset rows publish together or drop after Seal.
    void EndRoot(const DrawEffectReaders& readers, const std::uint8_t* base, const std::atomic<std::uint64_t>& inFlight) noexcept;
    // Drops this thread's sample, if any, for a root that did not complete.
    void AbandonRoot() noexcept;
    // Waits for any commit in progress, then refuses later ones: every sample
    // the report counts has its rows, and none lands after it.
    void Seal() noexcept;
    // Counts the sample a thread still holds when it exits, unless the probe is gone.
    static void AbandonAtThreadExit(DrawEffectProbe* probe) noexcept;

    // One consistent view: the rows and the offset totals come from one copy.
    DrawEffectSnapshot            Snapshot() const;
    DrawEffectCounts              Counts() const;
    std::vector<DrawEffectOffset> Offsets() const;
    // Writes the TOML report.
    void Write(std::ostream& out) const;

private:
    struct Changes
    {
        std::vector<std::uint64_t> count;
        std::vector<std::uint8_t>  before, after; // first change
    };

    // Reserves a sample against the total cap and the current swap's budget in
    // one step and returns the swap sequence the sample belongs to.
    bool reserve(std::uint64_t& sequence) noexcept;

    const DrawEffectConfig config_;

    std::atomic<std::uint64_t> roots_{ 0 }, selected_{ 0 }, rootStarts_{ 0 };
    std::atomic<std::uint64_t> gateSkipped_{ 0 }, gateUnreadable_{ 0 }, snapshotUnreadable_{ 0 }, tiledSelected_{ 0 };
    std::atomic<std::uint64_t> abandoned_{ 0 }, limited_{ 0 }, budgetSkipped_{ 0 }, budgetSwaps_{ 0 }, swapsSeen_{ 0 };
    std::atomic<std::uint64_t> limitHitSwap_{ kDrawEffectNoSwap };
    // Read without the lock to skip work once the cap is reached; changed under budgetMutex_.
    std::atomic<std::uint64_t> reserved_{ 0 };
    std::atomic<std::uint32_t> interval_;

    // The current swap sequence, the samples reserved in it and the roots seen
    // when it began change together, so a sample never records a new sequence
    // against the old budget. Guarded by budgetMutex_.
    std::mutex    budgetMutex_;
    std::uint64_t swap_ = 0, swapUsed_ = 0, swapStartRoots_ = 0;

    // Everything a finished sample adds, and the closed flag, are guarded by
    // mutex_, so a snapshot sees a sample's counters together with its rows.
    mutable std::mutex mutex_;
    bool               closed_          = false;
    std::uint64_t      sampledIsolated_ = 0, sampledConcurrent_ = 0, sampledWithChange_ = 0;
    std::uint64_t      tiledIsolated_ = 0, tiledConcurrent_ = 0, tiledWithChange_ = 0;
    std::uint64_t      firstSampleSwap_ = kDrawEffectNoSwap, lastSampleSwap_ = kDrawEffectNoSwap;
    // Indexed by byte offset in G. [0] untiled isolated, [1] untiled concurrent,
    // [2] tiled isolated, [3] tiled concurrent.
    std::array<Changes, 4> changes_;
};

// The process probe behind the native_draw_effect_probe cvar. A draw root costs
// one relaxed load of the pointer while no probe is open.
extern std::atomic<DrawEffectProbe*> drawEffectProbe;

inline void NoteDrawEffectSwap(std::uint64_t sequence) noexcept
{
    if (auto* probe = drawEffectProbe.load(std::memory_order_relaxed))
        probe->NoteSwap(sequence);
}

// Claims a new file now so a bad path fails at startup, then enables the probe.
bool OpenDrawEffectProbe(const std::filesystem::path& path, std::string& error);
// Disables the probe, seals it against in-progress samples, and writes the
// report once; later calls do nothing.
bool CloseDrawEffectProbe(std::string& error);

} // namespace rerevved::gpu::guest_d3d
