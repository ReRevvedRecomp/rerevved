#pragma once

#include "guest_render_capture.h"
#include "resolve_derive.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Lowerer 0x826A3048 (g63:13039-13816); caller g3:9292 fills the returned
//   buffers before committing.
// Entry/operation          | Guest register, word or branch fact
// r3/r4/r5                 | G / PM4 parameters.
// r6/r7/r8                 | Vertex count / index count / flags (bit 2: 32-bit
//   indices).
// r9/r10                   | Vertex stride bytes / index-out address.
// Vertex-out address       | Caller r1+84; 308(r1) after the 224-byte frame
//   (g63:13776).
// Commit 0x826A2FF0        | G+0x30 = G+0x3484 (g85:12931-12938).
// G+0x30F0 transient      | (r9 >> 2) & 0xFF; old byte restored at g63:13416.
// Stride change            | Versus G+0x2E38 with G+0x3198 set: G+0x10 gains
//   0x80000 (g63:13084-13106).
// Dirty flush              | Nonzero G+0x00/+0x08/+0x10/+0x18/+0x20 emit
//   through helpers and clear; G+0x30 is their cursor (g63:13109-13410).
// Soft-limit flush         | Cursor > G+0x38 calls 0x8269C588
//   (g63:13414-13425).
// 0x8269B818 allocations   | 16-byte aligned vertexDwords * 4 and indexDwords *
//   4 bytes (g63:13437, 13448).
// Allocation failure       | Either null result (g63:13452-13458, 13797-13809).
// Local ring cursor        | 18 words, or 27 with G+0x2ABC bit 0; never stored
//   (g63:13460-13769).
// Success stores           | G+0x18 |= 1; G+0x3484 PM4 end; G+0x3488/+0x348C
//   buffers; G+0x3490/+0x3494 dword counts; | both out words receive buffers,
//   r3 = 0 (g63:13772-13795); G+0x30 stays at the flush cursor.
// Consume stores           | Allocations/staging/out words, G+0x3484 = entry
//   cursor, G+0x18 |= 1; ring stores skipped.
// Consume dirty state      | Entry words retained as a superset; next
//   draw/Clear/Resolve Original flushes them.
// Host read set            | G+0x3484..0x3494 and root stack slot
//   (ObserveNativeDrawCommit, native_draw_dispatch.cpp); no skipped store is
//   read.
// StateDirty refusal       | Low dirty 0x1E0000 or stride patch invokes
//   0x826ADD88 (g111:13022), walking shader records via 0x826ADB98 (g90:13093).
// AllocMarker refusal      | G+0x2ABD & 0x20 makes 0x8269B818 return null
//   (g93:12775-12783).
// TileGrowth refusal       | Record does not fit; 0x8269C170 calls allocator
//   (g63:13643-13658).
// AllocPath refusal        | Pool object without fixed buffer uses indirect
//   0x826A6AB8 (g93:12821-12824).
// RingSpace refusal        | Flush words may pass soft G+0x38 or reach hard
//   G+0x34; or take passes G+0x3A58 with G+0x33B0 clear, | moving both limits
//   to cursor (g93:12840-12865); LowererFlushWords bounds flush words.
// AllocCapacity refusal    | Allocation may fail.
// AllocWait refusal        | Null-pool allocator would wait for GPU at
//   0x8269B158.

inline constexpr std::uint32_t kLowererEntryAddress = 0x826A3048;

constexpr bool LowererConsumeEntry(std::uint32_t address) noexcept
{
    return address == kLowererEntryAddress;
}

// The slot of the vertex out word in the caller's frame, from the entry r1.
inline constexpr std::uint32_t kLowererVertexOutSlot = 84;

// The ring words of the PM4 draw: 13 common, 5 more untiled (g63:13465-13575,
// 13619-13637), and with the tile record 14 more (g63:13676-13768).
inline constexpr std::uint32_t kLowererPm4Words      = 18;
inline constexpr std::uint32_t kLowererPm4TiledWords = 27;

// The tiling byte (G+0x2ABC >> 24) of the tile record branch the consume models:
// the same exactly-0x21 shape as the IndexedBuffer consume (draw_effect_probe.h).
inline constexpr std::uint8_t kLowererTiledByte = 0x21;

// The tile record (g63:13663-13671): 16 bytes at the list cursor G+0x33C8.
inline constexpr std::uint32_t kLowererTileRecordBytes = 16;

// The dirty low word bits that make 0x826ADD88 run (g63:13159-13170).
inline constexpr std::uint32_t kLowererDirtyShaderMask = 0x1E0000;

// The highest device word the watch reads: G+0x3A58, the pool threshold.
inline constexpr std::uint32_t kLowererDeviceSpan = 0x3A5C;

// The registers the body uses, from the entry context.
struct LowererEntry
{
    std::uint32_t g           = 0; // r3
    std::uint32_t vertexCount = 0; // r6
    std::uint32_t indexCount  = 0; // r7
    std::uint32_t flags       = 0; // r8
    std::uint32_t stride      = 0; // r9
    std::uint32_t indexOut    = 0; // r10: the address of the index out word
    std::uint32_t stack       = 0; // r1
    std::uint32_t pcr         = 0; // r13, the guest KPCR
};

// The sizes of the two takes (g63:13055-13081).
struct LowererDraw
{
    std::uint32_t vertexDwords = 0, indexDwords = 0; // r19, r18
    std::uint32_t vertexBytes = 0, indexBytes = 0;   // what 0x8269B818 passes on: the dwords times 4
    std::uint8_t  strideByte = 0;                    // (r9 >> 2) & 0xFF
    bool          indices32  = false;                // flags & 4
};

// r19 is the low 32 bits of the signed product r6 * r9 divided by 4; r18 is r7
// for 32-bit indices, else (r7 + 1) / 2 (16-bit indices, two to a dword).
LowererDraw DeriveLowererDraw(const LowererEntry& entry) noexcept;

// The guest words the model reads, at the entry and again at the return.
struct LowererWatch
{
    std::array<std::uint64_t, 5> dirty{};                                                                          // G+0x00, +0x08, +0x10, +0x18, +0x20
    std::uint64_t                mask   = 0;                                                                       // G+0x28: 0x826AC8C8 runs when it meets the G+0x10 word (g63:13175-13190)
    std::uint32_t                cursor = 0, limit34 = 0, limit38 = 0;                                             // G+0x30, G+0x34, G+0x38
    std::uint32_t                fence      = 0;                                                                   // G+0x2A9C
    std::uint32_t                pendingEnd = 0, vertexDest = 0, indexDest = 0, vertexDwords = 0, indexDwords = 0; // G+0x3484..0x3494
    // The vertex out word: the slot in the caller's frame, the address it
    // names and the word there; and the index out word.
    std::uint32_t vertexOutAddress = 0, vertexOut = 0, indexOut = 0;
    std::uint8_t  tilingByte = 0, allocByte = 0, stateByte = 0; // G+0x2ABC, G+0x2ABD, G+0x2ABE
    std::uint32_t strideWord   = 0;                             // G+0x2E38: its top byte is the recorded stride byte
    std::uint32_t vertexShader = 0;                             // G+0x3198
    std::uint32_t targetCount  = 0;                             // G+0x31CC
    std::uint32_t bracket = 0, bracketNext = 0;                 // G+0x33B0, G+0x33B4
    std::uint32_t tileChunk = 0, tileCursor = 0, tileLimit = 0; // G+0x33C4, G+0x33C8, G+0x33CC
    // The allocator: the pool object G+0x34CC with its base and size (+152, +156),
    // the position, used count, wrap position and threshold, and the null pool
    // allocator's words (read only without a pool object, as ReadResolveWatch).
    std::uint32_t poolObject = 0, poolBase = 0, poolSize = 0, poolPos = 0, poolUsed = 0, wrapPos = 0, poolThreshold = 0;
    std::uint32_t sideEnd = 0, wrapLimit = 0, segment = 0, progress = 0;
};

// After device reach check (stack_guard.h), test pool pointer G+0x34CC
// (object+152/+156), progress pointer G+0x2A90 (block+4), words named by r1+84
// and r10. Tile G+0x33C8 header only; skip record.
template <class Read>
bool LowererStackObjectsOverlap(const Read& read, const StackReach& reach, const LowererEntry& entry)
{
    // Pool pointer metadata reaches+152/+156; progress reaches+4. Vertex-out
    // slot r1+84 is parameter pointer, the pointed word is read/stored;
    // indexOut r10 names another word. Tile record itself is skipped.
    return StackReachesAllocator(read, reach, entry.g) || StackReachesPointee(read, reach, std::uint64_t{ entry.stack } + kLowererVertexOutSlot, 0, 4) || reach.Overlaps(entry.indexOut, 4);
}

// False when a word is unreadable or an address would wrap. The vertex out slot
// must name a nonzero address.
bool ReadLowererWatch(const GuestWordReader& read, const LowererEntry& entry, LowererWatch& out);

// The upper bound of the ring words the flush groups can emit for these dirty
//   words (g63:13109-13410), or UINT64_MAX when they run 0x826ADD88 or the
//   render target count is past the loops' bound. Each helper's words per run
//   of consecutive set bits (a run is a header and a payload): 0x826ACFF8 on
//   G+0x00 and G+0x08  4 + 16 per bit: up to 3 filler words that align the
//   payload, the header, and four 16-byte lines per bit (g169:12999-13136)
//   0x826AC8C8 when the mask G+0x28 meets G+0x10   32 + 5n, n the target count
//   (g202:13191-13594, RingWordsAc8c8) 0x826ACC60 on G+0x10 bits 0..11, 12..16,
//   21..41 and 42..57, G+0x18 bits 34..54, G+0x20 bits 0..37 and 38..45    1 +
//   1 per bit (g71:12144-12260) 0x826ACEA0 on the G+0x18 low word   2 + 6 per
//   bit, and 4 words after the last run (g92:13099-13297) 0x826ACD20 on G+0x20
//   bits 49..54   13 words, then 2 + 4 per bit (g209:13175-13410) 0x826ACC60 on
//   G+0x20 bit 56 with the mask -16777216: a run of 40 bits, 41 words
//   (g63:13387-13406) Bits outside these groups stay set until the next flush,
//   which the helpers do not read.
std::uint64_t LowererFlushWords(const LowererWatch& entry) noexcept;

// Why the consume has no exit state; the order of the tests is the order of the
// enumerators after None.
enum class LowererExitRefusal : std::uint8_t
{
    None,
    Tiling,        // the tiling byte is neither 0 nor kLowererTiledByte
    StateDirty,    // 0x826ADD88 would run (see above)
    AllocMarker,   // G+0x2ABD & 0x20 at the entry
    TileGrowth,    // the tile list cannot take the record
    AllocPath,     // a pool object without a fixed buffer
    RingSpace,     // Original could flush
    AllocCapacity, // an allocation may fail
    AllocWait,     // the null pool allocator would wait
    // stack_guard.h supplies stack reach checks before LowererExitProbe reads
    // device/object words.
    StackOverlap,
};
inline constexpr std::size_t                                       kLowererExitRefusalCount = static_cast<std::size_t>(LowererExitRefusal::StackOverlap) + 1;
inline constexpr std::array<const char*, kLowererExitRefusalCount> kLowererExitRefusalNames{
    "none",
    "tiling",
    "state_dirty",
    "alloc_marker",
    "tile_growth",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
    "stack_overlap",
};

// The words the consume leaves, from the entry registers and words.
struct LowererExitState
{
    std::array<std::uint64_t, 5> dirty{};                                // as at the entry, with G+0x18 |= 1
    std::uint32_t                limit34 = 0, limit38 = 0;               // G+0x34, G+0x38: a fixed pool lowers them
    std::uint32_t                poolPos = 0, poolUsed = 0, wrapPos = 0; // G+0x3A44, G+0x3A54, G+0x3A4C
    std::uint32_t                pendingEnd = 0;                         // G+0x3484: the entry cursor
    std::uint32_t                vertexDest = 0, indexDest = 0;          // G+0x3488, G+0x348C: the two buffers, also the out words
    std::uint32_t                vertexDwords = 0, indexDwords = 0;      // G+0x3490, G+0x3494
    std::uint64_t                flushWords = 0;                         // the bound that proved Original cannot flush
    std::uint32_t                pm4Words   = 0;                         // the words Original adds after the flush groups
    bool                         tiled      = false;
};

// Lowerer allocation exit; success=Original completes without a flush, wait, or
//   failed allocation; failure: nullopt with refusal and why.
// allocation calls | source: g93:12757-12888; r4: 1; r5: alignment 16.
// fixed pool gate | +152 nonzero; 0x826A6848 (g100:13237-13311).
// fixed pool top | G+0x3A44 or the aligned pool end; buffer = (top - bytes) &
//   ~15.
// fixed pool failure | Below cursor plus 164.
// fixed pool stores | G+0x3A44 = buffer; lower G+0x34/G+0x38 by (top - buffer)
//   & ~3.
// null pool | call: 0x8269B618 ModelNullPoolAllocator; source:
//   g185:12921-13040.
// pool byte total | effect: G+0x3A54 += bytes; source: g93:12827-12835.
// take order | effect: the takes run one after another.
// first cursor | value: entry cursor plus flush words.
// fixed capacity | test: the fixed-pool failure is judged against the bound
//   (AllocCapacity).
// ring limits | effect: soft and hard limits are judged before a take lowers
//   them; ring checks of steps 2 and 3 precede allocations.
std::optional<LowererExitState> DeriveLowererExitState(const LowererEntry& entry, const LowererWatch& watch, LowererExitRefusal* refusal = nullptr, std::string* why = nullptr);

// One guest store of the exit state: a big-endian word.
struct LowererWrite
{
    std::uint32_t address = 0;
    std::uint32_t value   = 0;

    friend bool operator==(const LowererWrite&, const LowererWrite&) = default;
};

// The stores that take the guest from `watch` to `exit`: the words that change,
// in the order G+0x1C (the G+0x18 low half), G+0x34, G+0x38, G+0x3A44, G+0x3A4C,
// G+0x3A54, G+0x3484..0x3494, then the two out words, which are always stored.
std::vector<LowererWrite> LowererExitWrites(const LowererEntry& entry, const LowererWatch& watch, const LowererExitState& exit);

#if REREVVED_DEVELOPER
inline constexpr std::size_t kLowererExitWordCount = 22;

inline constexpr std::array<const char*, kLowererExitWordCount> kLowererExitWordNames{
    "result",
    "dirty_00",
    "dirty_08",
    "dirty_10",
    "dirty_18",
    "dirty_20",
    "limit_34",
    "limit_38",
    "pool_pos",
    "pool_used",
    "wrap_pos",
    "vertex_dest",
    "index_dest",
    "vertex_dwords",
    "index_dwords",
    "vertex_out",
    "index_out",
    "alloc_byte",
    "pm4_words",
    "ring_words",
    "tile_record",
    "fence",
};

// Exit bit i follows kLowererExitWordNames: dirty qwords zero except G+0x18=1;
// bound covers G+0x30 movement, PM4 words=(G+0x3484-cursor)/4. Tiled adds 16 to
// G+0x33C8, chunk unchanged; untiled leaves both.
unsigned LowererExitMismatches(const LowererExitState& predicted, const LowererWatch& entry, const LowererWatch& exit, std::uint32_t result) noexcept;

// The flush words Original emitted, from the two cursors, for the counts.
std::uint64_t LowererObservedFlushWords(const LowererWatch& entry, const LowererWatch& exit) noexcept;

// The native_lowerer_exit_check cvar (init-only, default off): compare the
// model with every lowerer call whose Original runs.
bool LowererExitCheckRequested() noexcept;

// Counts one comparison and returns nothing; `tiled` is the model's shape.
void NoteLowererExit(unsigned mismatch, bool tiled, std::uint64_t flushWords) noexcept;
// Counts a call whose entry state the model refuses.
void NoteLowererExitUnmodeled(LowererExitRefusal refusal) noexcept;
// Counts an entry whose words could not be read.
void NoteLowererExitUnreadable() noexcept;

struct LowererExitCounts
{
    std::uint64_t                                       ok = 0, tiled = 0, tiledOk = 0, unreadable = 0;
    std::array<std::uint64_t, kLowererExitWordCount>    mismatch{};
    std::array<std::uint64_t, kLowererExitRefusalCount> unmodeled{};
    std::uint64_t                                       flushWordsMax = 0, flushWordsSum = 0, flushWordsCalls = 0;
};

// Returns the counts since the previous take and resets them.
LowererExitCounts TakeLowererExitCounts() noexcept;
// Lowerer diagnostics report exit status, tiled/unreadable state, flush totals,
// mismatches, fences, and unmodeled reasons.
std::string FormatLowererExitCounts(const LowererExitCounts& counts);
#else
// The check cvar exists only in the developer build.
constexpr bool LowererExitCheckRequested() noexcept
{
    return false;
}
#endif

} // namespace rerevved::gpu::guest_d3d
