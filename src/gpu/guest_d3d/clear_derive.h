#pragma once

#include "guest_render_capture.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// The guest's default clear color, four floats copied to the body frame when
// the caller passes no color pointer (g158:12998-13015).
inline constexpr std::uint32_t kClearDefaultColor = 0x8208EBE0;

// Frames: 0x826A2970 uses 112 bytes, 0x826A22F8 adds 256; r1 is entry -368 at
// 0x826A1D90, with exit 0x826A2960 (g88:13009, g158:12984-12987,
// g216:12901-12904, g158:13923-13924).
inline constexpr std::uint32_t kClearFrameSize = 368;
// The default color is copied to r1+112 of that body frame (g158:13004-13015).
inline constexpr std::uint32_t kClearBodyColorOffset = 112;

// The most calls a list holds. A clear makes at most four (one per color slot).
inline constexpr std::size_t kClearMaxCalls = 8;

// The registers of Clear (0x826A2970) at entry: r3 the device, r4 the flags, r5
// the rectangle (left, top, right, bottom words) or 0, r6 the color pointer
// (four floats) or 0, f1 the depth, r8 the stencil, and r1.
struct ClearEntry
{
    std::uint32_t sp      = 0;
    std::uint32_t g       = 0;
    std::uint32_t flags   = 0;
    std::uint32_t rect    = 0;
    std::uint32_t color   = 0;
    double        depth   = 0;
    std::uint32_t stencil = 0;
    std::uint32_t pcr     = 0; // r13, the guest KPCR
};

// Per-target clear 0x826A1D90 takes flags r4, slot r5, rect r6-r9, color r10,
// depth f1, and stencil at r1+92.
struct ClearCall
{
    std::uint32_t                flags = 0;
    std::uint32_t                index = 0;
    std::array<std::int32_t, 4>  rect{};
    std::uint32_t                colorPointer = 0;
    std::array<std::uint32_t, 4> color{}; // the four words at colorPointer; zero unless ColorUsed()
    std::uint64_t                depthBits = 0;
    std::uint32_t                stencil   = 0;

    // The call clears color when its slot's flag bit is set (g158:13832-13837);
    // only then are the color words compared.
    bool ColorUsed() const noexcept
    {
        return index < 4 && ((flags >> index) & 1) != 0;
    }

    friend bool operator==(const ClearCall&, const ClearCall&) = default;
};

// The calls of one Clear, in order.
struct ClearCalls
{
    std::array<ClearCall, kClearMaxCalls> calls{};
    std::size_t                           count    = 0;
    std::size_t                           overflow = 0; // calls beyond the capacity
    // The body got past its early exits (g158:13025-13029, 13158-13165) and runs
    // its flush groups, even when it makes no call. False for a Clear that
    // leaves the device untouched.
    bool reachedFlush = false;

    void Add(const ClearCall& call) noexcept
    {
        if (count < calls.size())
            calls[count++] = call;
        else
            ++overflow;
    }

    std::size_t Total() const noexcept
    {
        return count + overflow;
    }
};

// The fctiwz of 0x826A22F8's viewport words (g158:13038-13059): NaN and
// negative overflow give INT_MIN, a value of INT_MAX or more gives INT_MAX,
// the rest truncate toward zero.
std::int32_t ClearFloatToInt32(float value) noexcept;

// Derive calls to 0x826A1D90 from Clear entry/G using bodies g216:12892-13061,
// g158:12972-13932. nullopt/why:unreadable, wrap, null rect with no target, or
// default color slot not filled by aligned store.
std::optional<ClearCalls> DeriveClearCalls(const GuestWordReader& read, const ClearEntry& entry, std::string* why = nullptr);

#if REREVVED_DEVELOPER
// 0x826A1D90 observation reads r3-r10, r1, f1, stencil r1+92, and color words;
// unreadable input returns why.
bool ObserveClearCall(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::uint32_t stack, double depth, ClearCall& out, std::string* why = nullptr);

inline constexpr std::size_t kClearFieldCount = 7;

inline constexpr std::array<const char*, kClearFieldCount> kClearFieldNames{
    "count",
    "index",
    "flags",
    "rect",
    "color",
    "depth",
    "stencil",
};

// Bit 0 marks list-length difference; other bits follow kClearFieldNames across
// shared calls. Always compare color pointers; compare color words only when
// both calls clear color.
unsigned ClearMismatches(const ClearCalls& derived, const ClearCalls& observed) noexcept;

// The cvar native_clear_derive_check.
bool ClearDeriveCheckRequested() noexcept;
// The cvar native_clear_effect_check.
bool ClearEffectCheckRequested() noexcept;
#else
// The check cvars exist only in the developer build.
constexpr bool ClearDeriveCheckRequested() noexcept
{
    return false;
}

constexpr bool ClearEffectCheckRequested() noexcept
{
    return false;
}
#endif

// Clear exit 0x826A2960 dirty qwords:G+0x10/+0x18/+0x20. G+0x2ABD bits:0x20
// alloc failure, 0x04 past early exits, 0x02 ring switch
// 0x8269C588(g202:12997-13001).
inline constexpr std::size_t kClearDirtyWords = 3;

inline constexpr std::uint8_t kClearAllocFailedBit = 0x20, kClearExitBit = 0x04, kClearSwitchBit = 0x02;

// The tile table of the tiled path holds at most this many tiles here; a
// larger G+0x31CC is not modeled.
inline constexpr std::uint32_t kClearMaxTiles = 32;

// The tile loop selects a tile through the bits (3 << 2i) of the mask G+0x31A4,
// and slw gives zero for a shift of 32 or more, so at most this many tiles are
// ever selected (g88:13704-13708).
inline constexpr std::uint32_t kClearMaxLoopTiles = 16;

// Clear exit effects; inputs: entry watch, derived effects, side buffers;
//   fields: dirty words and byte G+0x2ABD.
// final | effect: (entry & keep) | set | (allocated ? setIfAllocated : 0) per
//   word.
// keep | effect: flush-group contribution; source: g158:13166-13390.
// set | effect: prelude ORs; source: g158:13657-13725.
// set | effect: color-branch ORs; source: g158:13818-13830.
// set | effect: exit ORs; source: g158:13913-13924.
// set | effect: 0x826A1D90 ORs before allocation; source:
//   g88:13172-13176,13375-13388.
// setIfAllocated | effect: 0x826A1800 ORs after successful allocation; source:
//   g96:13268-13306.
struct ClearEffects
{
    bool        reachedFlush = false;
    bool        color        = false; // the body takes its color branch (flags & 0xF)
    bool        allocates    = false; // some call reaches 0x826A1800, so a successful allocation sets `setIfAllocated`
    bool        tiled        = false; // the device is tiled for 0x826A1D90 (ClearDeviceTiled): every call of the body sees it the same
    std::size_t calls = 0, stencilCalls = 0, fastCalls = 0;
    std::size_t untiledCalls = 0, tiledBlockCalls = 0, tiledLoopCalls = 0, tiledNoneCalls = 0;
    // The tiles the loops of all calls select. Each emits two ring words and
    // makes one call to 0x826A1AF0 (g88:13715-13772).
    std::size_t                                 loopTiles = 0;
    std::array<std::uint64_t, kClearDirtyWords> keep{}, set{}, setIfAllocated{};
    std::uint8_t                                byteSet = 0;
};

// Clear model reads G+0x2ABC/G+0x2ABF, G+0x31B8, G+0x31A4, G+0x31CC, target
// formats, and G+0x2880.
std::optional<ClearEffects> DeriveClearEffects(const GuestWordReader& read, const ClearEntry& entry, const ClearCalls& calls, std::string* why = nullptr);

// The words the check compares, read at the entry and again at the exit.
struct ClearWatch
{
    std::array<std::uint64_t, kClearDirtyWords> dirty{};
    std::uint8_t                                allocByte = 0, stateByte = 0;        // G+0x2ABD, G+0x2ABE
    std::uint32_t                               ring = 0, allocated = 0, cursor = 0; // G+0x30, G+0x3A54, G+0x3A44
    // Allocator 0x8269B818 reads ring G+0x34/G+0x38, threshold G+0x3A58, target
    // count G+0x31CC, bracket G+0x33B0, and pool G+0x34CC.
    std::uint32_t limit34 = 0, limit38 = 0, poolThreshold = 0, targetCount = 0, bracket = 0, poolObject = 0, poolBase = 0, poolSize = 0;
    // Null-pool-only reads, zero otherwise:wrap position G+0x3A4C, end
    // G+0x3A3C, wrap limit G+0x3A50, segment G+0x3A48, next G+0x33B4,
    // progress[[G+0x2A90]+4]. Wrap takes advance position.
    std::uint32_t wrapPos = 0, sideEnd = 0, wrapLimit = 0, segment = 0, bracketNext = 0, progress = 0;
    // The tiling byte G+0x2ABC and the predicate G+0x31AC, which classify the
    // tiling shape (ClearTilingShape). No function of the Clear reads the
    // predicate, so it does not change the exit state.
    std::uint32_t tilingByte = 0, predicate = 0;
};

// Tiling classes report words read from G+0x33B0 and the device tile
// mask/table; model follows guest words, not class labels.
inline constexpr unsigned kClearShapeLoop      = 1;  // the device is tiled for 0x826A1D90: the tile loop runs
inline constexpr unsigned kClearShapeBracket   = 2;  // the bracket word G+0x33B0 is set
inline constexpr unsigned kClearShapeByte10    = 4;  // tiling byte bit 0x10
inline constexpr unsigned kClearShapeState     = 8;  // tiling byte bit 0x20 and the bracket word disagree
inline constexpr unsigned kClearShapePredicate = 16; // the predicate G+0x31AC is set

inline constexpr std::size_t kClearShapeCount = 5;

#if REREVVED_DEVELOPER
inline constexpr std::array<const char*, kClearShapeCount> kClearShapeNames{
    "loop",
    "bracket",
    "byte10",
    "state",
    "predicate",
};
#endif

// A shape counts as tiled when the tile loop runs or the bracket is set.
constexpr bool ClearShapeTiled(unsigned shape) noexcept
{
    return (shape & (kClearShapeLoop | kClearShapeBracket)) != 0;
}

// The span of the device the watch reads: G+0x3A58, the pool threshold, is its
// highest word. The pool object and the progress block are read through their
// own addresses.
inline constexpr std::uint32_t kClearDeviceSpan = 0x3A5C;

// False when a word is unreadable or an address would wrap.
bool ReadClearWatch(const GuestWordReader& read, std::uint32_t g, ClearWatch& out);

// The kClearShape bits of a call: whether the tile loop runs (effects.tiled),
// the bracket word, tiling byte bit 0x10, bit 0x20 against the bracket word, and
// the predicate. The predicate bit is set only for a tiled shape.
unsigned ClearTilingShape(const ClearEffects& effects, const ClearWatch& entry) noexcept;

// Tiled 0x826A1D90: G+0x2ABC has 0x10, or 0x20 with bound slots equal saved
// G+0x31B8+4i; unbound matches all(g88:13389-13487). Unreadable/wrap returns
// false, tiled unchanged, failed address; tileByte gets byte.
bool ClearDeviceTiled(const GuestWordReader& read, std::uint32_t g, bool& tiled, std::uint32_t* tileByte = nullptr, std::uint32_t* failed = nullptr);

// The word 0x826A1AF0 reads to choose between one strip and edge strips plus an
// interior (g22:13006-13013).
inline constexpr std::uint32_t kClearStripWord = 0x82F1AC14;

// The memory the skipped Clear Original is checked against (stack_guard.h): the
//   stack reach [limit, r1), since no path touches the caller's frame (the
//   stencil word at the body's r1+92 and the color copy at r1+112 lie in the
//   body's own frame, g158:13004-13015), and what the model reads or writes:
//   device     [G, G + kClearDeviceSpan): the dirty words, ring cursor and
//   limits, tiling and mode words, bound surfaces G+0x3098 and G+0x30A8,
//   viewport and scissor, tile tables, bracket, pool words (G+0x3A58 the
//   highest), all of which the exit writes among rectangle  r5, 16 bytes, when
//   not null color      r6, 16 bytes, when not null surfaces   the extent word
//   +0x24 of the bound surface G+0x3098 names and of the depth surface G+0x30A8
//   names (the null-rectangle form) allocator  the pool object G+0x34CC names
//   (+152, +156) and the progress block G+0x2A90 names (+4) statics    the
//   default color kClearDefaultColor (16 bytes) and the strip word
//   kClearStripWord (4 bytes), words of the title image The pool's fixed buffer
//   and the side buffers it holds are neither read nor written by the model.
//   The registers' stencil r8 and depth f1 are values. The entry function's own
//   frame is 112 bytes (stwu r1,-112(r1), g216:12901); a call with fewer bytes
//   between r1 and the stack limit is refused. The body it calls has a frame of
//   its own, outside the model's no-overflow domain.
inline constexpr StackFootprint             kClearStackFootprint{ 0, kClearDeviceSpan, 112 };
inline constexpr std::array<StackObject, 2> kClearStackStatics{ { { kClearDefaultColor, 16 }, { kClearStripWord, 4 } } };

// True for unknown stack reach or any listed byte overlapped. Test reach/device
// before reads beyond KPCR; test pointer word before reading it, then its
// object, so stack stores cannot change modeled reads.
bool ClearStackOverlaps(const GuestWordReader& read, const ClearEntry& entry);

// Clear side-buffer allocations; unit: byte counts; order: one Clear's calls to
//   0x826A1800.
// allocation amount | effect: 84 bytes per strip.
// untiled call | call: 0x826A1D90 -> 0x826A1AF0; grid: (G+0x2880 >> 16) & 3
//   multisample mode; source: g22:13105-13237.
// edge strips | effect: grid splits into up to four edge strips, with one
//   0x826A1800 call when any exists.
// interior | effect: second one-strip call; fallback: one strip when the grid
//   leaves no interior or kClearStripWord is zero.
// tiled call | effect: one call per selected tile, with the clipped rectangle
//   moved to the tile origin.
struct ClearSideBuffers
{
    static constexpr std::size_t kCapacity = 2 * kClearMaxLoopTiles * kClearMaxCalls;

    std::size_t                          count = 0;
    std::array<std::uint32_t, kCapacity> bytes{};
};

// Replay tiled call loop g88:13631-13789: signed rect intersections at
// G+0x31D0+16i selected by(3<<2i)&G+0x31A4; 0x826A1AF0 moves intersection by
// G+0x32C0+8i/+0x32C4+8i(g88:13760-13772).
std::optional<ClearSideBuffers> DeriveClearSideBuffers(const GuestWordReader& read, const ClearEntry& entry, const ClearCalls& calls, std::string* why = nullptr);

// Every watched word at the exit of a Clear whose exit is exact: the call the
// consume skips. Order of kClearExitWordNames.
struct ClearExitState
{
    // Tile blockA(g88:13522-13630) is counted by DeriveClearEffects but not
    // replayed; DeriveClearExitState refuses it. Unreadable, overflow or tile
    // count>kClearMaxTiles returns nullopt/why.
    std::array<std::uint64_t, kClearDirtyWords> dirty{};
    std::uint8_t                                allocByte = 0;
    std::uint32_t                               limit34 = 0, limit38 = 0;  // G+0x34, G+0x38: each side buffer take lowers them
    std::uint32_t                               poolPos = 0, poolUsed = 0; // G+0x3A44, G+0x3A54
    std::uint32_t                               wrapPos = 0;               // G+0x3A4C: the null pool allocator's wrap position
    unsigned                                    shape   = 0;               // the tiling words the call saw (kClearShape bits); not a guest word
};

#if REREVVED_DEVELOPER
inline constexpr std::size_t kClearExitWordCount = 9;

inline constexpr std::array<const char*, kClearExitWordCount> kClearExitWordNames{
    "dirty10",
    "dirty18",
    "dirty20",
    "byte2abd",
    "limit34",
    "limit38",
    "pool_pos",
    "pool_used",
    "wrap_pos",
};
#endif

// Why no exact exit state exists for a Clear. The enumerators follow the order
// in which DeriveClearExitState tests them.
enum class ClearExitRefusal : std::uint8_t
{
    None,
    TiledBlock,    // some call takes the tile block A (g88:13522-13630), which the model leaves to a census (none seen so far)
    EarlyExit,     // the body leaves before its flush groups, so nothing changes
    AllocMarker,   // G+0x2ABD & 0x20 at the entry: every allocation returns at once
    AllocPath,     // a pool object without a fixed buffer: the indirect allocator 0x826A6AB8, which the host does not model
    RingSpace,     // Original could flush: its ring words may pass a limit, or a take passes the pool threshold
    AllocCapacity, // the allocator would fail: the fixed buffer cannot hold a side buffer below the ring, or the null pool allocator has no room
    AllocWait,     // the null pool allocator would wait in 0x8269B158 for the GPU to leave the segment
    // ClearStackOverlaps includes unknown reach or stack-held model
    // words/objects. DeriveClearExitState tests no stack; consume verdict and
    // exit probe check before any other guest word read.
    StackOverlap,
};
inline constexpr std::size_t                                     kClearExitRefusalCount = static_cast<std::size_t>(ClearExitRefusal::StackOverlap) + 1;
inline constexpr std::array<const char*, kClearExitRefusalCount> kClearExitRefusalNames{
    "none",
    "tiled_block",
    "early_exit",
    "alloc_marker",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
    "stack_overlap",
};

// The ring words the Clear can emit for these effects and this entry state, or
//   UINT64_MAX when the render target count is beyond the loops' bound. Every
//   ring check of the body compares the cursor with G+0x38 and calls the flush
//   0x8269C588 when it is past it (g158 before each of its blocks, g88 before
//   its blocks, g96:12898-12911), and 0x826ACC60 compares a run's end with
//   G+0x34 (g71:12189-12203), so a call can flush only when the cursor, grown
//   by every word it can emit, passes a limit. The words, with n the render
//   target count G+0x31CC and k the calls to 0x826A1D90: 0x826AC8C8 once in the
//   body and once per call   32 + 5n each (g202:13191-13594) the six flush
//   groups of the body                 popcount + runs of the entry dirty bits
//   (RingFlushGroupWords) the three blocks after them                      34 +
//   15 + 14 = 63 (g158) each 0x826A1D90 itself                           2 + 4
//   + 6 = 12 (g88) each 0x826A1800                                  22 (g96)
//   each tile the loops select                       2 (g88:13715-13759)
//   0x8268D3A0 once                                  12 + 7n (g153)
std::uint64_t ClearMaxRingWords(const ClearEffects& effects, const ClearSideBuffers& sides, const ClearWatch& entry) noexcept;

// Clear exit-state prediction; inputs: derived effects, side buffers, watched
//   entry words.
// dirty | value: (entry & keep) | set | (a call allocated ? setIfAllocated :
//   0).
// allocByte | value: entry with bit 0x04 set.
// side-buffer bytes | call: 0x826A1800 -> 0x8269B818; effect: G+0x3A54 receives
//   bytes on every path; source: g93:12826-12839.
// fixed pool | gate: pool object; call: 0x826A6848; top: G+0x3A44 or base +
//   aligned size; buffer: (top - bytes) & ~3; effect: lowers G+0x34 and G+0x38
//   by (top - that) & ~3; source: g100:13237-13331.
// null pool | call: 0x8269B618 ModelNullPoolAllocator; bytes: the same;
//   alignment: 4; effect: moves neither limit and stores G+0x3A44 forward or
//   G+0x3A4C wrap; source: g185:12899-13183.
// take order | effect: each take sees the stores of the preceding take.
// bracket | gate: G+0x33B0 set; effect: a take past the pool threshold does not
//   move limits; source: g93:12840-12841.
// bracket threshold | effect: not a refusal reason; null-pool takes use the
//   bracket terms.
// admission | effect: only calls whose allocations all succeed and whose
//   Original cannot flush are modeled; otherwise nullopt sets refusal and why.
// refusal tile | condition: the tile block A.
// refusal early | condition: the body leaves early.
// refusal marker | condition: the allocation-failure marker is set at entry.
// refusal fixed-pool | condition: a pool object has no fixed buffer.
// refusal ring | condition: ring words may pass a limit.
// refusal threshold | condition: takes pass G+0x3A58 with bracket clear;
//   effect: allocator moves limits to the cursor and the next ring check
//   flushes; source: g93:12840-12865.
// refusal fixed take | condition: the last take would land below cursor plus
//   164; source: g100:13276-13287.
// refusal null take | condition: a null-pool take fails; source:
//   g185:12956-13028.
// refusal wait | condition: a take would wait in 0x8269B158; source:
//   g206:12440-12501.
// refusal order | effect: tests follow the order of ClearExitRefusal.
std::optional<ClearExitState> DeriveClearExitState(const ClearEffects& effects, const ClearSideBuffers& sides, const ClearWatch& entry, ClearExitRefusal* refusal = nullptr, std::string* why = nullptr);

#if REREVVED_DEVELOPER
// Bit i is set when word i (kClearExitWordNames order) of `exit` differs.
unsigned ClearExitMismatches(const ClearExitState& predicted, const ClearWatch& exit) noexcept;
#endif

// One guest store of the exit state: a big-endian word, or the byte G+0x2ABD.
struct ClearWrite
{
    std::uint32_t address = 0;
    std::uint32_t value   = 0;
    bool          byte    = false;

    friend bool operator==(const ClearWrite&, const ClearWrite&) = default;
};

// Store changed exit words in kClearExitWordNames order. Dirty qwords are
// big-endian 32-bit halves, high first; store only halves that change.
std::vector<ClearWrite> ClearExitWrites(std::uint32_t g, const ClearWatch& entry, const ClearExitState& exit);

#if REREVVED_DEVELOPER
inline constexpr std::size_t kClearEffectFieldCount = 4;

inline constexpr std::array<const char*, kClearEffectFieldCount> kClearEffectFieldNames{
    "dirty10",
    "dirty18",
    "dirty20",
    "byte2abd",
};

struct ClearEffectResult
{
    bool                                        early = false, earlyChanged = false; // no flush: nothing may change
    bool                                        compared      = false;               // an exact comparison ran
    bool                                        allocFailed   = false;               // an allocation failed during the call: not compared
    bool                                        markerAtEntry = false;               // G+0x2ABD & 0x20 was set at the entry, so no allocation succeeds
    bool                                        switchBit     = false;               // the exit byte carries bit 0x02 that the entry did not
    unsigned                                    mismatch      = 0;                   // bit i is field i of kClearEffectFieldNames
    std::array<std::uint64_t, kClearDirtyWords> expect{};
    std::uint8_t                                expectByte   = 0;
    bool                                        stateChanged = false; // G+0x2ABE differs
    std::int64_t                                ringDelta = 0, allocDelta = 0, cursorDelta = 0;
};

// No flush:dirty qwords/G+0x2ABD unchanged. New 0x20 alloc failure is not
// compared; entry 0x20 suppresses postalloc ORs(g93:12774-12783). New
// 0x02=switch; G+0x2ABE/allocation/ring/cursor only recorded.
ClearEffectResult CompareClearEffects(const ClearEffects& predicted, const ClearWatch& entry, const ClearWatch& exit) noexcept;
#endif

// One Clear call between its entry and the exit of its body 0x826A22F8. The
// stack keeps the frames of the Clears in flight, so a consume can refuse a
// Clear nested in another; the developer build also keeps the comparison
// state of the check cvars.
struct ClearProbe
{
    bool          live        = true;  // false once a hook saw another device; the probe then keeps its frame only
    bool          deriveCalls = true;  // native_clear_derive_check was on at the entry
    bool          effects     = false; // native_clear_effect_check was on at the entry
    std::uint32_t frame       = 0;     // r1 inside the body
#if REREVVED_DEVELOPER
    ClearEntry                  entry;
    std::optional<ClearCalls>   derived;
    std::string                 why; // the unmodeled reason when `derived` is empty, else the first unreadable observed word
    ClearCalls                  observed;
    bool                        observedUnreadable = false; // a call's stencil or color words could not be read
    std::optional<ClearEffects> predicted;
    ClearWatch                  entryWatch;
    std::string                 effectsWhy; // the unmodeled reason when `predicted` is empty
    // The exact exit state of the consume model for the same entry state, or
    // the reason it does not exist.
    std::optional<ClearExitState> exit;
    ClearExitRefusal              exitRefusal  = ClearExitRefusal::None;
    bool                          stackOverlap = false; // ClearStackOverlaps at the entry, tested before any guest word of the probe is read
    std::size_t                   sideTakes    = 0;     // the 0x826A1800 calls the model counted, for the mismatch log
#endif
};

// How many dropped probes belonged to each check.
struct ClearDropCounts
{
    unsigned derive = 0, effects = 0;
};

// Clear probes are outermost-first; stale stacked frames are dropped when
// nested calls leave without their exit hook.
class ClearProbeStack
{
public:
    static constexpr std::size_t kDepth = 4;

    // Push drops stale below-entry frames, counts derive/effect drops, and
    // refuses a full probe stack.
    unsigned Push(std::uint32_t entrySp, ClearProbe probe, ClearDropCounts* drops = nullptr);
    // The topmost probe with this frame, with everything above it dropped and
    // counted in `dropped`; nullptr, with nothing changed, when none matches.
    ClearProbe* Find(std::uint32_t frame, unsigned& dropped, ClearDropCounts* drops = nullptr);
    // As Find, then removes the probe too.
    std::optional<ClearProbe> Pop(std::uint32_t frame, unsigned& dropped, ClearDropCounts* drops = nullptr);

    std::size_t Size() const noexcept
    {
        return count_;
    }

    void Clear() noexcept
    {
        count_ = 0;
    }

private:
    std::array<ClearProbe, kDepth> slots_;
    std::size_t                    count_ = 0;
};

#if REREVVED_DEVELOPER
// Comparison returns a mismatch mask; unreadable or unmodeled input is
// unsupported and zero is exact.
unsigned NoteClearDerive(const std::optional<ClearCalls>& derived, const ClearCalls& observed, bool observedReadable) noexcept;
// Counts probes dropped unpaired: a call that never reached its exit hook, a
// full probe stack, or a hook whose device is not the probe's.
void NoteClearUnpaired(unsigned count) noexcept;

struct ClearDeriveCounts
{
    std::uint64_t                               ok = 0, unsupported = 0, unpaired = 0;
    std::array<std::uint64_t, kClearFieldCount> mismatch{};
};

// Returns the counts since the previous take and resets them.
ClearDeriveCounts TakeClearDeriveCounts() noexcept;
// Clear diagnostics report success, mismatch categories, unsupported calls, and
// pairing counts.
std::string FormatClearDeriveCounts(const ClearDeriveCounts& counts);

// Counts one effect comparison: exact ok or the differing fields, early ok or
// changed, an allocation failure, and the record-only changes. Path coverage
// counts every comparison of a call that got past its early exits.
void NoteClearEffect(const ClearEffects& predicted, const ClearEffectResult& result) noexcept;
// Counts a call whose effects the derivation does not model.
void NoteClearEffectUnsupported() noexcept;
// Counts effect probes dropped unpaired.
void NoteClearEffectUnpaired(unsigned count) noexcept;

struct ClearEffectCounts
{
    std::uint64_t                                     exactOk = 0, earlyOk = 0, earlyChanged = 0, allocFailed = 0, unsupported = 0, unpaired = 0;
    std::array<std::uint64_t, kClearEffectFieldCount> mismatch{};
    std::uint64_t                                     pathUntiled = 0, pathTiledBlock = 0, pathTiledLoop = 0, pathTiledNone = 0;
    std::uint64_t                                     color = 0, depthOnly = 0, stencil = 0, fast = 0, flushNoCalls = 0;
    std::uint64_t                                     stateChanged = 0, markerAtEntry = 0, switchBit = 0;
    std::int64_t                                      allocBytesSum = 0, ringDeltaSum = 0, cursorDeltaSum = 0;
};

// Returns the counts since the previous take and resets them.
ClearEffectCounts TakeClearEffectCounts() noexcept;
// Counters cover exact/early/alloc/unsupported/unpaired outcomes, tile paths,
// forms, masks, allocations, ring, and cursor deltas.
std::string FormatClearEffectCounts(const ClearEffectCounts& counts);

// ClearExitMismatches compares exit state; a refused model exit is unmodeled.
void NoteClearExit(unsigned mismatch, unsigned shape = 0) noexcept;
void NoteClearExitUnmodeled(ClearExitRefusal refusal) noexcept;

struct ClearExitCounts
{
    std::uint64_t                                     ok    = 0;
    std::uint64_t                                     tiled = 0, tiledOk = 0; // modeled exits of a tiled shape; the ok ones are also in ok
    std::array<std::uint64_t, kClearShapeCount>       shapes{};               // modeled exits of a tiled shape, by kind (a shape can have several)
    std::array<std::uint64_t, kClearExitWordCount>    mismatch{};
    std::array<std::uint64_t, kClearExitRefusalCount> unmodeled{};
};

// Returns the counts since the previous take and resets them.
ClearExitCounts TakeClearExitCounts() noexcept;
// " clear_exit_ok=N clear_exit_tiled=N clear_exit_tiled_ok=N
// clear_exit_mismatch_dirty10=N ... clear_exit_mismatch_wrap_pos=N", then
// " clear_exit_tiled_<kind>=N" and " clear_exit_unmodeled_<reason>=N" for nonzero counts.
std::string FormatClearExitCounts(const ClearExitCounts& counts);

// Call formatting includes flags, index, rect, color words, depth, stencil, and
// +N overflow.
std::string FormatClearCall(const ClearCall& call);
std::string FormatClearCalls(const ClearCalls& calls);
#endif

} // namespace rerevved::gpu::guest_d3d
