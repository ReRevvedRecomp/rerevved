#pragma once

#include "guest_render_capture.h"
#include "ring_words.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Window writer 0x8268D3A0 (g153:12634-13092): guest call/read/store map used
//   by the consume model.
// Fact                         | Guest behavior / source
// Setter callers               | 0x8268F9C0 (g188:13025), 0x82690040
//   (g167:12996), 0x826922D0 tail call (g160:13243), 0x82698720 (g208:13450).
// Other callers                | Clear body g158:13912; Resolve body
//   g167:13639, g167:14913.
// Entry registers              | r3 G, r4 x0, r5 y0, r6 x1, r7 y1
//   (g153:12650-12656).
// First ring check             | c = G+0x30 > soft limit G+0x38 calls flush
//   0x8269C588 (g153:12656-12668).
// Tiled form                   | G+0x2ABC bit 0x10, or bit 0x20 with each of
//   five G+0x3098+4i surfaces equal to G+0x31B8+4i or zero (g153:12670-12760).
// Plain form                   | Otherwise four ring words; r3 = c+12
//   (g153:12762-12804).
// Tiled body                   | Two words, then seven per n = G+0x31CC tiles,
//   clamped to rect G+0x31D0+16i with offsets G+0x32C0+8i.
// Tiled ring checks            | After each tile (g153:12806-12994).
// Tiled tail                   | Six more words if G+0x2ABF bit 0x20 and tiling
//   bit 0x40; four more for predicates G+0x31A4/G+0x31A8 (g153:12997-13083).
// Tiled return                 | r3 = last tile's 3 << 2i, or c+8 with no tile.
// Final cursor                 | G+0x30 = cursor after the words (g153:13085).
// Original store set           | Ring stwu chain, G+0x30, stack saves below
//   caller r1 (__savegprlr_21 g29:15204 and frame back chain).
// Consume                      | No host reads the ring: omit those stores,
//   keep G+0x30 = c and set r3; other volatile registers are dead at every
//   caller.
// TileCount refusal            | Above kWindowWriterMaxTiles the loop is
//   unbounded.
// StackOverlap refusal         | Unknown [limit,r1), or overlap with device
//   [G,G+0x3340) / probe-read ring words: prologue saves corrupt body/epilogue
//   inputs.
// RingSpace refusal            | c > G+0x38 at entry, or c+4*(2+7n) > G+0x38
//   for tiled n>=1 (last tile check; tail has none), or words pass 2^32.

inline constexpr std::uint32_t kWindowWriterEntryAddress = 0x8268D3A0;

constexpr bool WindowWriterConsumeEntry(std::uint32_t address) noexcept
{
    return address == kWindowWriterEntryAddress;
}

// The loop reads the tile arrays G+0x31D0 and G+0x32C0 for at most the render
// target bound the other consumes use.
inline constexpr std::uint32_t kWindowWriterMaxTiles = kRingMaxTargetCount;

// The words of the forms (g153:12762-12804, 12806-13083).
inline constexpr std::uint32_t kWindowWriterPlainWords       = 4;
inline constexpr std::uint32_t kWindowWriterTiledHeaderWords = 2;
inline constexpr std::uint32_t kWindowWriterTileWords        = 7;
inline constexpr std::uint32_t kWindowWriterExtraWords       = 6;
inline constexpr std::uint32_t kWindowWriterTailWords        = 4;
// The most one call stores: 12 + 7n for n = kWindowWriterMaxTiles (RingWords8d3a0).
inline constexpr std::uint32_t kWindowWriterMaxWords = kWindowWriterTiledHeaderWords + kWindowWriterTileWords * kWindowWriterMaxTiles + kWindowWriterExtraWords + kWindowWriterTailWords;

// The highest device word the watch reads: the offsets of the last tile.
inline constexpr std::uint32_t kWindowWriterDeviceSpan = 0x3340;

// The tiling byte bits (G+0x2ABC) and the flag bit of G+0x2ABF the body tests.
inline constexpr std::uint8_t kWindowWriterTilingForceBit = 0x10; // tiled form at once
inline constexpr std::uint8_t kWindowWriterTilingBoundBit = 0x20; // tiled form when the bound surfaces match
inline constexpr std::uint8_t kWindowWriterTilingExtraBit = 0x40; // with the flag below: the extra block
inline constexpr std::uint8_t kWindowWriterFlagExtraBit   = 0x20; // G+0x2ABF

// The registers the body uses, from the entry context.
struct WindowWriterEntry
{
    std::uint32_t g   = 0; // r3
    std::uint32_t x0  = 0; // r4
    std::uint32_t y0  = 0; // r5
    std::uint32_t x1  = 0; // r6
    std::uint32_t y1  = 0; // r7
    std::uint32_t sp  = 0; // r1, the stack pointer at the entry
    std::uint32_t pcr = 0; // r13, the guest KPCR
};

// Window writer frame 176 bytes (g153:12642/g153:12643/g153:13090/g29:15204);
// stack_guard.h rejects shorter reach.
inline constexpr StackFootprint kWindowWriterStackFootprint{ 0, kWindowWriterDeviceSpan, 176 };

// True when the thread's stack reach holds a byte of the device words of `g`, or is
// unknown. Tested before any guest word beyond the KPCR is read; `reach` receives
// the reach of the call.
bool WindowWriterStackOverlaps(const GuestWordReader& read, const StackFrame& frame, std::uint32_t g, StackReach& reach);

// True when the `words` ring words Original stores after the cursor, [cursor+4,
// cursor+4+4*words), share a byte with the reach. Only the exit probe reads them.
inline bool WindowWriterRingOverlaps(const StackReach& reach, std::uint32_t cursor, std::uint32_t words) noexcept
{
    return reach.Overlaps(std::uint64_t{ cursor } + 4, std::uint64_t{ words } * 4);
}

// One tile: the rect at G+0x31D0 + 16i (left, top, right, bottom) and the
// offsets at G+0x32C0 + 8i (x, y), as signed 32-bit words.
struct WindowWriterTile
{
    std::array<std::uint32_t, 4> rect{};
    std::array<std::uint32_t, 2> offset{};

    friend bool operator==(const WindowWriterTile&, const WindowWriterTile&) = default;
};

// Window-writer model reads only control-flow-reached
// cursor/limit/mode/bound/tile words; the probe also reads ring contents.
struct WindowWriterWatch
{
    std::uint32_t                                       cursor = 0, limit34 = 0, limit38 = 0; // G+0x30, G+0x34 (content reads only), G+0x38
    std::uint32_t                                       mode = 0;                             // the word at G+0x2ABC: the tiling byte on top, G+0x2ABF in the low byte
    std::array<std::uint32_t, 5>                        bound{}, applied{};                   // G+0x3098 + 4i, G+0x31B8 + 4i; read when the mode bits are exactly 0x20
    std::uint32_t                                       tileCount     = 0;                    // G+0x31CC; read for the tiled form
    std::uint32_t                                       predicateMask = 0, predicateWord = 0; // G+0x31A4, G+0x31A8; content reads of the tiled form
    std::array<WindowWriterTile, kWindowWriterMaxTiles> tiles{};                              // content reads of the tiled form, the first tileCount entries

    std::uint8_t TilingByte() const noexcept
    {
        return static_cast<std::uint8_t>(mode >> 24);
    }

    std::uint8_t FlagByte() const noexcept
    {
        return static_cast<std::uint8_t>(mode);
    }
};

enum class WindowWriterForm : std::uint8_t
{
    Plain,
    Tiled,
};

// The form Original takes for these words (step 2). Reads only the words the
// control flow reads: the bound pairs when the mode bits are exactly 0x20.
WindowWriterForm ClassifyWindowWriterForm(const WindowWriterWatch& watch) noexcept;

// Window content reads G+0x34, predicate words, and tiled arrays when tile
// bound admits.
bool ReadWindowWriterWatch(const GuestWordReader& read, std::uint32_t g, bool content, WindowWriterWatch& out);

// Why the consume has no exit state; the order of the tests is the order of the
// enumerators after None (RingSpace is tested first for the entry check, again
// after the tile count for the tile checks).
enum class WindowWriterRefusal : std::uint8_t
{
    None,
    TileCount, // more tiles than kWindowWriterMaxTiles
    RingSpace, // Original could flush
    // Window-writer stack overlap checks device/object reach before any guest
    // word read; unknown reach remains unmodeled.
    StackOverlap,
};
#if REREVVED_DEVELOPER
inline constexpr std::size_t kWindowWriterRefusalCount = static_cast<std::size_t>(WindowWriterRefusal::StackOverlap) + 1;

inline constexpr std::array<const char*, kWindowWriterRefusalCount> kWindowWriterRefusalNames{
    "none",
    "tile_count",
    "ring_space",
    "stack_overlap",
};
#endif

// What Original leaves, apart from the ring words themselves.
struct WindowWriterExitState
{
    WindowWriterForm form   = WindowWriterForm::Plain;
    std::uint32_t    tiles  = 0;     // the tiles the loop runs; zero for a plain call
    bool             extra  = false; // the six-word block of a tiled call
    std::uint32_t    words  = 0;     // ring words stored
    std::uint32_t    cursor = 0;     // G+0x30 at the return: the entry cursor plus four bytes a word
    std::uint32_t    result = 0;     // r3, zero extended
};

// The exit state of a call that Original completes without a flush, or nullopt
// with `refusal` and a short reason in `why`.
std::optional<WindowWriterExitState> DeriveWindowWriterExitState(const WindowWriterWatch& watch, WindowWriterRefusal* refusal = nullptr, std::string* why = nullptr);

// The ring words Original stores after the cursor, in order, for a derived
// call. A tiled call needs a content watch.
std::vector<std::uint32_t> WindowWriterRingWords(const WindowWriterEntry& entry, const WindowWriterWatch& watch, const WindowWriterExitState& exit);

#if REREVVED_DEVELOPER
inline constexpr std::size_t kWindowWriterExitWordCount = 6;

inline constexpr std::array<const char*, kWindowWriterExitWordCount> kWindowWriterExitWordNames{
    "result",
    "cursor",
    "limit_34",
    "limit_38",
    "state",
    "ring_content",
};

// Window-writer mismatch bit i names kWindowWriterExitWordNames; state excludes
// cursor and limits.
unsigned WindowWriterExitMismatches(const WindowWriterExitState&      predicted,
                                    const WindowWriterWatch&          before,
                                    const WindowWriterWatch&          after,
                                    std::uint64_t                     result,
                                    const std::vector<std::uint32_t>& expected,
                                    const std::vector<std::uint32_t>& observed) noexcept;

// The ring words Original stored, from the two cursors, for the counts: zero
// when the cursor moved back or by a part of a word.
std::uint64_t WindowWriterObservedWords(const WindowWriterWatch& before, const WindowWriterWatch& after) noexcept;

// The native_window_writer_exit_check cvar (init-only, default off): compare the
// model with every window writer call whose Original runs.
bool WindowWriterExitCheckRequested() noexcept;

// Counts one comparison; `tiled` is the model's form.
void NoteWindowWriterExit(unsigned mismatch, bool tiled, std::uint64_t ringWords) noexcept;
// Counts a call whose entry state the model refuses.
void NoteWindowWriterExitUnmodeled(WindowWriterRefusal refusal) noexcept;
// Counts an entry or exit whose words could not be read.
void NoteWindowWriterExitUnreadable() noexcept;

struct WindowWriterExitCounts
{
    std::uint64_t                                         ok = 0, tiled = 0, tiledOk = 0, unreadable = 0;
    std::array<std::uint64_t, kWindowWriterExitWordCount> mismatch{};
    std::array<std::uint64_t, kWindowWriterRefusalCount>  unmodeled{};
    std::uint64_t                                         ringWordsMax = 0, ringWordsSum = 0, calls = 0;
};

// Returns the counts since the previous take and resets them.
WindowWriterExitCounts TakeWindowWriterExitCounts() noexcept;
// Window-writer diagnostics report success, tiled/unreadable status, ring
// totals, result mismatches, and reasons.
std::string FormatWindowWriterExitCounts(const WindowWriterExitCounts& counts);
#else
// The check cvar exists only in the developer build.
constexpr bool WindowWriterExitCheckRequested() noexcept
{
    return false;
}
#endif

} // namespace rerevved::gpu::guest_d3d
