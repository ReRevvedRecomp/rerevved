#pragma once

#include "indexed_buffer_gate.h"
#include "native_draw_hook.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

class IRingStubMemory;

// Consume 0x826A39F8 skips Original: only ring, cursor G+0x30 and dirty
// effects; host numbering/ledger/submission unchanged. Tiled 0x21 adds G+0x33C8
// records/masks; dirty guards prove 0x826ADD88 host-visible words unchanged.

// The body calls 0x826ADD88, the only path to the render-register caches, the
// shader-record id and the variant records, only when the low word of the
// dirty qword at G+0x10 has a bit of this mask.
inline constexpr std::uint32_t kIndexedBufferDirtyLowOffset = 0x14;
// Accepted dirty roots replay five zero qwords G+0/+8/+0x10/+0x18/+0x20. Tiled
// 0x21 only records masks/list growth; those stores feed GPU tile masks, not
// host readers.
inline constexpr std::uint32_t kIndexedBufferDirtyLowMask = 0x1E0000;

// The tiled tripwires, read from G at entry. All must hold for a TiledRecord
//   root: G+0x33B0 != 0                BeginTiling set it from G+0x33B8
//   (g123:12816-12817) and EndTiling clears it, so it marks an open bracket
//   G+0x31AC == 0                the bracket state the static scope requires
//   clear byte G+0x2ABD & 0x20 == 0    no allocation failure (0x8269B818 sets
//   it, g93:12876) The mode byte shares its word with the tiling byte at
//   G+0x2ABC.
inline constexpr std::uint32_t kIndexedBufferBracketOffset = 0x33B0;
inline constexpr std::uint32_t kIndexedBufferPendingOffset = 0x31AC;
inline constexpr std::uint32_t kIndexedBufferModeWord      = 0x2ABC;
inline constexpr std::uint32_t kIndexedBufferModeFailed    = 0x20;

// The words are the big-endian values at G+0x33B0, G+0x31AC and G+0x2ABC.
constexpr bool IndexedBufferTiledStateHolds(std::uint32_t bracketWord, std::uint32_t pendingWord, std::uint32_t modeWord) noexcept
{
    return bracketWord != 0 && pendingWord == 0 && ((modeWord >> 16) & kIndexedBufferModeFailed) == 0;
}

enum class IndexedBufferTiledState : std::uint8_t
{
    Holds,
    Tripped,
    Unreadable,
};

IndexedBufferTiledState CheckIndexedBufferTiledState(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics) noexcept;

// The dirty guards, read from G at entry (g111 = 0x826ADD88): G+0x2ABE bit 0x40
//   clear      the recorded alternate VS flag; 0x826ADD88 clears it only on
//   some paths (g111:13488, 13620) no RB_MODECONTROL toggle     with VS
//   G+0x3198 set, G+0x2954 & 7 already 5 when PS G+0x3194 is 0, or 4 when PS is
//   set and the dirty word has 0x100000 (stores g111:13133, 13317); a toggle
//   also rewrites G+0x2884, which Resolve turns into clear words no alternate
//   depth-only VS   with G+0x2ABC bit 0x40 (the Z prepass) and a VS, VS header
//   word +872 has no 0x20; only that path can set G+0x2ABE bit 0x40
//   (g111:13369, 13586) Without a VS 0x826ADD88 returns after the HIZ derive,
//   which feeds ring words.
inline constexpr std::uint32_t kIndexedBufferPixelShaderOffset  = 0x3194;
inline constexpr std::uint32_t kIndexedBufferVertexShaderOffset = 0x3198;
inline constexpr std::uint32_t kIndexedBufferModeControlOffset  = 0x2954;
inline constexpr std::uint32_t kIndexedBufferVertexFlagsOffset  = 872;
inline constexpr std::uint32_t kIndexedBufferDirtyPixel         = 0x100000;
inline constexpr std::uint32_t kIndexedBufferRecordedShader     = 0x4000;     // G+0x2ABE bit 0x40 in the word at G+0x2ABC
inline constexpr std::uint32_t kIndexedBufferZPrepass           = 0x40000000; // G+0x2ABC bit 0x40 in that word

// Dirty vertex-fetch patch; gate: dirty bit 0x80000; function=0x826ADD88.
// patch calls | effect: patches VS vertex-fetch microcode in place; source:
//   g111:13454-13523 calls 0x826ADC48; g155:12794-12902 patches through
//   0x826AD150.
// command-buffer gate | condition: G+0x2ABE bit 0x80 clear and byte G+0x2ABC
//   bit 0x80 clear; else=patch goes to the command buffer or is skipped;
//   source: g111:13454-13459,13501-13511.
// declaration gate | condition: VD = G+0x2E2C != 0 and VS header word +872 bit
//   0x40 clear; else=no patch; source: g111:13485-13499.
// variant gate | condition: the VS cache record for the variant misses; source:
//   g155:12809-12846.
// variant record | address: VS+40+416*v; fields: tag +0 and stride bytes +8 and
//   +16.
// cache hit | condition: tag(rec) equals tag at VD+48 and (((BE64(rec+8) ^
//   BE64(G+0x30F0)) & BE64(VD+32)) | ((BE64(rec+16) ^ BE64(G+0x30F8)) &
//   BE64(VD+40))) == 0.
// variant v | value: 1 without a PS when VS header word +872 has 0x20, else 0;
//   source: g111:13096-13109.
// miss-only tests | excludes in-flight timestamp rec+24 and interpolator flag,
//   which can only withhold the patch.
// wrapper stride | store=(stride >> 2) & 0xFF in byte G+0x30F0, the
//   most-significant byte of the first word; source: g63:13087-13091.
// host derivation | source: HostPatchedVertexDigest uses shader templates and
//   draw state, reads no guest memory, and lets
//   native_consume_dirty_vertex_fetch skip the patch.
inline constexpr std::uint32_t kIndexedBufferVertexDeclOffset   = 0x2E2C;
inline constexpr std::uint32_t kIndexedBufferStreamOffset       = 0x30F0;
inline constexpr std::uint32_t kIndexedBufferDeclMaskOffset     = 32;
inline constexpr std::uint32_t kIndexedBufferDeclTagOffset      = 48;
inline constexpr std::uint32_t kIndexedBufferFetchCacheOffset   = 40;
inline constexpr std::uint32_t kIndexedBufferFetchCacheStride   = 416;
inline constexpr std::uint32_t kIndexedBufferVertexAlternate    = 0x20;       // VS header word +872
inline constexpr std::uint32_t kIndexedBufferVertexNoPatch      = 0x40;       // VS header word +872
inline constexpr std::uint32_t kIndexedBufferFetchCommandBuffer = 0x8000;     // G+0x2ABE bit 0x80
inline constexpr std::uint32_t kIndexedBufferFetchNoPatch       = 0x80000000; // G+0x2ABC bit 0x80

enum class IndexedBufferDirtyState : std::uint8_t
{
    Holds,
    Recorded,
    ModeToggle,
    ZPrepass,
    VertexFetch,
    Unreadable,
};

// `dirtyLow` is the big-endian word at G+0x14 as the verdict read it. `stream0`
// is the byte a user-pointer wrapper's nested call stores at G+0x30F0 first.
IndexedBufferDirtyState CheckIndexedBufferDirtyState(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics, std::uint32_t dirtyLow, std::optional<std::uint8_t> stream0 = std::nullopt) noexcept;

// The memory the skipped IndexedBuffer Original is checked against
//   (stack_guard.h): the stack reach [limit, r1) with nothing above r1 (no path
//   of g148 or the frames below it touches the caller's frame), and what the
//   model reads: device   [G, G + kIndexedBufferDeviceSpan): the tiling and
//   mode word G+0x2ABC, the dirty word G+0x14, the shader words G+0x3194 and
//   G+0x3198, G+0x2954, the declaration word G+0x2E2C, the stream bytes
//   G+0x30F0..0x30FF, G+0x2E38 and the tiled tripwires G+0x31AC and G+0x33B0,
//   the highest VS       the vertex shader G+0x3198 names: the header word
//   +872, and the fetch cache record of each variant, VS+40 and VS+456 (24
//   bytes each: the tag and the stride bytes) decl     the vertex declaration
//   G+0x2E2C names: the masks, tag and fields at +32..+51 The pixel shader word
//   G+0x3194 is only tested for zero. The consume stores the five dirty qwords,
//   preflighting all destinations first. A user-pointer wrapper's stride slot
//   r1+84 is its parameter. The entry function's own frame is 208 bytes (stwu
//   r1,-208(r1), g148:13093); a call with fewer bytes between r1 and the stack
//   limit is refused (stack_guard.h).
inline constexpr std::uint32_t  kIndexedBufferDeviceSpan = kIndexedBufferBracketOffset + 4;
inline constexpr StackFootprint kIndexedBufferStackFootprint{ 0, kIndexedBufferDeviceSpan, 208 };

// True when an object of the list above, found through the device words of `g`, shares a byte with the stack reach.
// Called after the reach and the device words are tested, so each pointer word is known not to be in the reach.
template <class Read>
bool IndexedBufferStackObjectsOverlap(const Read& read, const StackReach& reach, std::uint32_t g)
{
    const std::uint64_t vertex = std::uint64_t{ g } + kIndexedBufferVertexShaderOffset;
    if (StackReachesPointee(read, reach, vertex, kIndexedBufferVertexFlagsOffset, 4))
        return true;
    for (std::uint32_t variant = 0; variant < 2; ++variant)
        if (StackReachesPointee(read, reach, vertex, kIndexedBufferFetchCacheOffset + variant * kIndexedBufferFetchCacheStride, 24))
            return true;
    return StackReachesPointee(read, reach, std::uint64_t{ g } + kIndexedBufferVertexDeclOffset, kIndexedBufferDeclMaskOffset, 20);
}

// Why a root ran its Original with the cvar on. The enumerators follow the
// order in which the verdict tests them.
enum class IndexedBufferFallback : std::uint8_t
{
    Dispatcher,       // no live dispatcher observes the call
    Nested,           // the call is nested in another observed draw
    Stubs,            // ring stubs are off or have not checked this device
    StackOverlap,     // the thread's stack reach holds a device word or an object the model reads, or is unknown
    Tiling,           // the tiling byte at G+0x2ABC admits the tile-record branch
    Unreadable,       // a required word could not be read, or a dirty replay destination is not writable
    StateDirty,       // a dirty bit of kIndexedBufferDirtyLowMask with the dirty cvar off
    DirtyRecorded,    // a dirty root with G+0x2ABE bit 0x40 set
    DirtyMode,        // a dirty root whose Original would toggle RB_MODECONTROL
    DirtyZPrepass,    // a dirty Z-prepass root whose VS has the alternate depth-only variant
    DirtyVertexFetch, // a root with dirty bit 0x80000 whose Original would patch the VS vertex fetch microcode, with the dirty vertex fetch cvar off
    TiledState,       // a tiled root with a tripped tiled-state tripwire
    Diagnostics,      // a diagnostic consumer needs the Original body
    Reservation,      // a user-pointer reservation or failure outcome is not established
};
inline constexpr std::size_t kIndexedBufferFallbackCount = static_cast<std::size_t>(IndexedBufferFallback::Reservation) + 1;

inline constexpr std::array<std::string_view, kIndexedBufferFallbackCount> kIndexedBufferFallbackNames{
    "dispatcher",
    "nested",
    "stubs",
    "stack_overlap",
    "tiling",
    "unreadable",
    "state_dirty",
    "dirty_recorded",
    "dirty_mode",
    "dirty_zprepass",
    "dirty_vertex_fetch",
    "tiled_state",
    "diagnostics",
    "reservation",
};

// The cvar native_consume_indexed_buffer while ring stub services are published;
// a consume does nothing without them.
bool IndexedBufferConsumeRequested() noexcept;
// The cvar native_consume_indexed_buffer_tiled, effective only with the one above.
bool IndexedBufferTiledConsumeRequested() noexcept;
// The cvar native_consume_indexed_buffer_dirty, effective only with the base cvar.
bool IndexedBufferDirtyConsumeRequested() noexcept;
// The cvar native_consume_dirty_vertex_fetch, shared by the IndexedBuffer entry and the
// user-pointer wrappers; effective only with the dirty cvar of the entry that reads it.
bool DirtyVertexFetchConsumeRequested() noexcept;

// Empty verdict permits consume. Order:dispatcher, nested, stubs, stack before
// reads, tiling, dirty/guards, tiled tripwires, diagnostics. Set
// tiled/dirty/vertexFetch for admitted subsets; sample effect probe here.
std::optional<IndexedBufferFallback> IndexedBufferConsumeVerdict(const NativeDrawScope& scope, std::uint32_t graphics, bool diagnostics, bool* tiled = nullptr, bool* dirty = nullptr, bool* vertexFetch = nullptr) noexcept;

struct NativeDrawRootView;

// Validate all ten word destinations and reads before any dirty replay store.
bool PreflightDrawDirtyClear(const NativeDrawRootView& root, std::uint32_t graphics) noexcept;
// Replay the five zero-qword stores in Original order, then the UP success
// rearm (G+0x18 = 1) when requested. Requires a successful preflight and lease.
void ReplayDrawDirtyClear(const NativeDrawRootView& root, std::uint32_t graphics, bool rearm) noexcept;

// Which subsets of the consumed roots a consumable root falls in.
struct ConsumeShape
{
    bool tiled       = false; // a TiledRecord root
    bool dirty       = false; // a dirty bit of kIndexedBufferDirtyLowMask
    bool vertexFetch = false; // dirty, and admitted by the dirty vertex fetch cvar although Original would patch the VS
};

// User stride differs when (stride >> 2) & 0xFF != G+0x2E38 and G+0x3198 is
// nonzero; g63:13087-13107/g151:12803-12824 then call 0x826ADD88.
inline constexpr std::uint32_t kIndexedBufferStrideByteOffset = 0x2E38;
inline constexpr std::uint32_t kIndexedBufferStrideDirty      = 0x80000;

struct EntryStride
{
    bool          inStack = false; // `value` is the guest address of the stride word, not the stride
    std::uint32_t value   = 0;
};

// The objects of an entry beyond the shader and declaration objects of
// IndexedBufferStackObjectsOverlap: `overlaps` is called with the reach, the open
// root and `context` once the device words are tested, and returns true to refuse.
struct StackObjects
{
    bool (*overlaps)(const StackReach& reach, const NativeDrawRootView& root, const void* context) noexcept = nullptr;
    const void* context                                                                                     = nullptr;
};

// Shared consume tests exclude diagnostics. Stack/object guards precede other
// guest reads; wrapper stride may add dirty state before dirty guards. Success
// returns open root and shape; refusal clears shape.
std::optional<IndexedBufferFallback> ConsumeStateVerdict(const NativeDrawScope& scope,
                                                         std::uint32_t          graphics,
                                                         bool                   tiledCvar,
                                                         bool                   dirtyCvar,
                                                         bool                   vertexFetchCvar,
                                                         const EntryStride*     stride,
                                                         const StackFrame&      frame,
                                                         const StackFootprint&  footprint,
                                                         const StackObjects&    objects,
                                                         NativeDrawRootView&    root,
                                                         ConsumeShape&          shape) noexcept;

// Consumed dirty-vertex-fetch observer runs before hook return and must not
// store guest memory. stride is wrapper entry stride, null for IndexedBuffer
// whose stream 0 stride is G+0x30F0 byte.
struct VertexFetchConsume
{
    const NativeDrawRootView* root     = nullptr;
    std::uint32_t             graphics = 0;
    const EntryStride*        stride   = nullptr;
};

using VertexFetchConsumeObserver = void (*)(const VertexFetchConsume&) noexcept;
// Installs `observer`, or removes it with null.
void SetVertexFetchConsumeObserver(VertexFetchConsumeObserver observer) noexcept;
// Calls the installed observer, if any.
void NoteVertexFetchConsume(const VertexFetchConsume& consume) noexcept;
// The same with the current root, which is built only when an observer is installed.
void NoteVertexFetchConsume(std::uint32_t graphics, const EntryStride* stride) noexcept;

// True when the hook must skip Original. With the cvar off or without published
// ring stub services it returns false and counts nothing; otherwise it counts
// the verdict.
bool ConsumeIndexedBuffer(const NativeDrawScope& scope, const PPCContext& ctx, bool diagnostics) noexcept;

#if REREVVED_DEVELOPER
struct IndexedBufferConsumeCounts
{
    std::uint64_t                                          consumed                 = 0; // includes the tiled and dirty ones
    std::uint64_t                                          consumedTiled            = 0; // includes tiled dirty ones
    std::uint64_t                                          consumedDirty            = 0; // includes tiled dirty ones
    std::uint64_t                                          consumedDirtyVertexFetch = 0; // dirty roots the dirty vertex fetch cvar admitted
    std::array<std::uint64_t, kIndexedBufferFallbackCount> fallback{};
};

// Returns the counts since the previous take and resets them.
IndexedBufferConsumeCounts TakeIndexedBufferConsumeCounts() noexcept;
// " draw_consumed_indexed_buffer=N", " draw_consumed_indexed_buffer_tiled=N",
// " draw_consumed_indexed_buffer_dirty=N", " draw_consumed_indexed_buffer_dirty_vertex_fetch=N", then
// " draw_fallback_indexed_buffer_<reason>=N" for nonzero counts.
std::string FormatIndexedBufferConsumeCounts(const IndexedBufferConsumeCounts& counts);
#endif

// Bracket header 0x33C0:chunk G+0x33C4, cursor+0x33C8 virtual(0x8269C170,
// g45:12856-12864). Chunk next word precedes 16-byte records from+4 to
// cursor(g148:13577-13606); interior pointers use C0000000, not header.
inline constexpr std::uint32_t kTileListChunkOffset  = 0x33C4;
inline constexpr std::uint32_t kTileListCursorOffset = 0x33C8;
inline constexpr std::uint32_t kTileRecordBytes      = 16;

struct TileListShape
{
    std::uint64_t records    = 0;     // whole records in the current chunk
    bool          empty      = false; // well formed with no record
    bool          misaligned = false; // the cursor is before the first record or off a record boundary
};

// A misaligned list counts as neither empty nor any records.
constexpr TileListShape ClassifyTileList(std::uint32_t chunk, std::uint32_t cursor) noexcept
{
    const std::uint64_t first = static_cast<std::uint64_t>(chunk) + 4;
    if (cursor < first || (cursor - first) % kTileRecordBytes != 0)
        return { 0, false, true };
    const std::uint64_t records = (cursor - first) / kTileRecordBytes;
    return { records, records == 0, false };
}

#if REREVVED_DEVELOPER
struct TileListCounts
{
    std::uint64_t records    = 0;
    std::uint64_t empty      = 0;
    std::uint64_t misaligned = 0;
};

// True when EndTiling should observe its list: a base consume and its tiled cvar
// are on, for the IndexedBuffer entry (with published ring stub services), the
// user-pointer wrappers (likewise) or the direct lowerer.
bool TileListObservationRequested() noexcept;
// Reads the list of the device `graphics` before EndTiling terminates it and
// counts it; an unreadable header counts as misaligned.
void ObserveTileList(IRingStubMemory& memory, std::uint32_t graphics) noexcept;
void NoteTileList(const TileListShape& shape) noexcept;
// Returns the counts since the previous take and resets them.
TileListCounts TakeTileListCounts() noexcept;
// " tiling_list_records=N tiling_list_empty=N tiling_list_misaligned=N".
std::string FormatTileListCounts(const TileListCounts& counts);
#else
constexpr bool TileListObservationRequested() noexcept
{
    return false;
}

inline void ObserveTileList(IRingStubMemory&, std::uint32_t) noexcept
{
}
#endif

} // namespace rerevved::gpu::guest_d3d
