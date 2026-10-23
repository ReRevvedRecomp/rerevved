#pragma once

#include "guest_render_capture.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// SetPredication(G, mask) at 0x8269CD20 (g171:12559-12772) stores the predicate
//   and its mask into the device, folds the predicate into bit 0 of the tiling
//   byte G+0x2ABC, records the mask in two more words and appends two ring
//   words. The exit state is a pure function of the argument r4, the old tiling
//   byte, the byte G+0x2F9B and the five bound surfaces with their BeginTiling
//   snapshots:
//  r4 != 0: G+0x31A4 = r4, G+0x31AC = 1, byte = old & 0xFE (g171:12712-12721).
//   r4 == 0: G+0x31AC = 0, G+0x31A4 = 0xFFFFFFFF, byte = (old & 0xFE) | t, with
//   t = 0 when old & 8, old & 4 or G+0x2F9B is nonzero; else 1 when old & 0x10;
//   else 0 when old & 0x20 is clear; else 1 when every bound word G+0x3098+4i
//   is zero or equals G+0x31B8+4i (g171:12581-12708). both:    G+0x31A8 = 0,
//   G+0x2AB4 = r4, G+0x2AB8 = 0 and the byte store (g171:12723-12736).
//  Then, when the cursor G+0x30 is above the soft limit G+0x38 (unsigned,
//   g171:12733-12743), Original calls the flush 0x8269C588 (g202:12873) first;
//   the cursor is then a different word, and it writes the ring words cursor+4
//   = 0xC0006000 and cursor+8 = the mask, stores cursor+8 to G+0x30 and returns
//   it in r3 (g171:12745-12760). The 18 call sites (g205:8898, 9311; g180:9104,
//   9133, 9331, 9353, 9480, 9502; g70:12794, 12908, 12946, 12992; g14:13014,
//   13060; g156:12614, 12660, 12684; g119:12710) read neither r3 nor r8..r12
//   nor cr0 and cr6 after the call.
//  The prologue also stores to the caller's stack (g171:12565-12572): the link
//   register at r1-8, r31 at r1-16 (eight bytes) and the old r1 at the new
//   frame r1-96. A consumed call omits them, so a stack that holds a device
//   word the model reads or writes (the byte G+0x2F9B, for one) would leave it
//   different from what Original sees. The check is against the thread's whole
//   stack reach [limit, r1), not the three stores, and against the device words
//   up to kSetPredicationDeviceSpan (stack_guard.h); see
//   SetPredicationStackOverlaps.

inline constexpr std::uint32_t kSetPredicationAddress = 0x8269CD20;
// The link register of the ring stub's own call from the pre-swap stub
// (ring_stub_hooks.cpp), which reads the cursor right after it.
inline constexpr std::uint32_t kSetPredicationPreSwapReturn = 0x826A4178;
// One past the last device byte the model reads: G+0x31B8 + 4 * 4 + 4.
inline constexpr std::uint32_t kSetPredicationDeviceSpan = 0x31CC;
inline constexpr std::size_t   kSetPredicationSlots      = 5;
// SetPredication guest-footprint fact table:
// Stack domain: the checked stack reach is [limit, r1); nothing above r1
//   participates.
// Device domain: the model reads/writes [G, G + kSetPredicationDeviceSpan),
//   with the highest read/write at G+0x31CB; it reads no pointed object.
// Ordinary consume: Original's ring stores at cursor+4 and cursor+8 are
//   omitted.
// Pre-swap consume: those ring stores are retained, and
//   SetPredicationRingOverlaps tests them.
// Frame: 96 bytes from stwu r1,-96(r1), g171:12569; it saves r12 at r1-8 and
//   r31 at r1-16 (g171:12565-12572).
// Refusal: a call with fewer bytes between r1 and the stack limit is refused.
inline constexpr StackFootprint kSetPredicationStackFootprint{ 0, kSetPredicationDeviceSpan, 96 };

// The first ring word Original writes (g171:12745-12751).
inline constexpr std::uint32_t kSetPredicationRingHeader = 0xC0006000;

// The device words, as offsets from G.
inline constexpr std::uint32_t kSetPredicationCursorOffset    = 0x30;   // ring cursor
inline constexpr std::uint32_t kSetPredicationLimitOffset     = 0x38;   // soft limit
inline constexpr std::uint32_t kSetPredicationBoundOffset     = 0x3098; // five bound surfaces
inline constexpr std::uint32_t kSetPredicationSnapshotOffset  = 0x31B8; // five BeginTiling snapshots
inline constexpr std::uint32_t kSetPredicationMaskOffset      = 0x31A4;
inline constexpr std::uint32_t kSetPredicationPredicateOffset = 0x31AC;
inline constexpr std::uint32_t kSetPredicationWord31A8Offset  = 0x31A8;
inline constexpr std::uint32_t kSetPredicationWord2AB4Offset  = 0x2AB4;
inline constexpr std::uint32_t kSetPredicationWord2AB8Offset  = 0x2AB8;
inline constexpr std::uint32_t kSetPredicationTilingOffset    = 0x2ABC; // one byte
inline constexpr std::uint32_t kSetPredicationFlagOffset      = 0x2F9B; // one byte

// The device words the model reads, and the old values of the words it stores.
struct SetPredicationWatch
{
    std::uint32_t                                   cursor = 0, limit = 0;
    std::uint8_t                                    tiling = 0; // the byte G+0x2ABC
    std::uint8_t                                    flag   = 0; // the byte G+0x2F9B
    std::array<std::uint32_t, kSetPredicationSlots> bound{}, snapshot{};
    std::uint32_t                                   predicate = 0, mask = 0, word31a8 = 0, word2ab4 = 0, word2ab8 = 0;

    friend bool operator==(const SetPredicationWatch&, const SetPredicationWatch&) = default;
};

// True when the thread's stack reach holds a byte of the device words of `g` the
// model reads or writes, or is unknown. Tested before any guest word beyond the
// KPCR is read. `reach` receives the reach of the call.
bool SetPredicationStackOverlaps(const GuestWordReader& read, const StackFrame& frame, std::uint32_t g, StackReach& reach);

// True when the ring words Original writes after the cursor, [cursor+4, cursor+12),
// share a byte with the reach. Used by the exit probe and pre-swap consume.
inline bool SetPredicationRingOverlaps(const StackReach& reach, std::uint32_t cursor) noexcept
{
    return reach.Overlaps(std::uint64_t{ cursor } + 4, 8);
}

// Reads the watched words of device `g`; false when any is unreadable or the
// device base is too high for the offsets.
bool ReadSetPredicationWatch(const GuestWordReader& read, std::uint32_t g, SetPredicationWatch& out);

// The words Original leaves, for the argument r4.
struct SetPredicationExit
{
    std::uint32_t predicate = 0, mask = 0, word31a8 = 0, word2ab4 = 0, word2ab8 = 0;
    std::uint8_t  tiling = 0;
    // Original calls the flush 0x8269C588 before its ring words: the cursor is
    // above the soft limit, so the cursor and ring words are not modeled.
    bool flush = false;

    friend bool operator==(const SetPredicationExit&, const SetPredicationExit&) = default;
};

SetPredicationExit DeriveSetPredicationExit(std::uint32_t r4, const SetPredicationWatch& entry) noexcept;

// One guest store of the exit state: a big-endian word, or the byte G+0x2ABC.
struct SetPredicationWrite
{
    std::uint32_t address = 0;
    std::uint32_t value   = 0;
    bool          byte    = false;

    friend bool operator==(const SetPredicationWrite&, const SetPredicationWrite&) = default;
};

struct SetPredicationWrites
{
    std::array<SetPredicationWrite, 9> list{};
    std::size_t                        count = 0;
};

// SetPredication stores change predicate, mask, byte, and three path words in
// Original order; pre-swap adds packets and cursor.
SetPredicationWrites SetPredicationExitWrites(std::uint32_t g, const SetPredicationWatch& entry, const SetPredicationExit& exit, bool preSwap = false) noexcept;

#if REREVVED_DEVELOPER
// The compared words of the exit probe; bit i of a mismatch mask is word i.
inline constexpr std::size_t kSetPredicationExitWordCount = 10;

inline constexpr std::array<std::string_view, kSetPredicationExitWordCount> kSetPredicationExitWordNames{
    "predicate",   // G+0x31AC
    "mask",        // G+0x31A4
    "word31a8",    // G+0x31A8
    "word2ab4",    // G+0x2AB4
    "word2ab8",    // G+0x2AB8
    "tiling",      // the byte G+0x2ABC
    "cursor",      // G+0x30 (without a flush)
    "ring_header", // the word at the entry cursor + 4 (without a flush)
    "ring_mask",   // the word at the entry cursor + 8 (without a flush)
    "result",      // r3, the new cursor (without a flush)
};

// The words Original left, read after it returned. The ring words are those at
// the entry cursor + 4 and + 8, read only when no flush ran.
struct SetPredicationObserved
{
    SetPredicationWatch watch;
    std::uint32_t       ringHeader = 0, ringMask = 0;
    std::uint32_t       result = 0; // r3
};

// Bit i is set when word i of kSetPredicationExitWordNames differs between the
// model (`predicted`, made from `entry`) and `observed`. With a predicted flush
// the last four words are not compared.
unsigned SetPredicationExitMismatches(const SetPredicationExit& predicted, const SetPredicationWatch& entry, const SetPredicationObserved& observed) noexcept;
#endif

} // namespace rerevved::gpu::guest_d3d
