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

// The copy words Resolve (0x8269E520) stores in the device shadow, which the
// hook at 0x8269E8A0 reads from G+0x2A18..0x2A24.
struct ResolveCopyWords
{
    std::uint32_t copyControl = 0; // G+0x2A18, RB_COPY_CONTROL
    std::uint32_t destBase    = 0; // G+0x2A1C, RB_COPY_DEST_BASE
    std::uint32_t destPitch   = 0; // G+0x2A20, RB_COPY_DEST_PITCH
    std::uint32_t destInfo    = 0; // G+0x2A24, RB_COPY_DEST_INFO

    friend bool operator==(const ResolveCopyWords&, const ResolveCopyWords&) = default;
};

inline constexpr std::size_t kResolveWordCount = 4;

inline constexpr std::array<const char*, kResolveWordCount> kResolveWordNames{
    "copy_control",
    "dest_base",
    "dest_pitch",
    "dest_info",
};

// The per-format halfword table Resolve indexes with the destination format
// (g167:13200), and the zero point it substitutes for a null destination point
// (g167:13278-13280).
inline constexpr std::uint32_t kResolveFormatTable  = 0x8208EA10;
inline constexpr std::uint32_t kResolveDefaultPoint = 0x820D783C;

// Derive copy words at 0x8269E8A0 from entry r3..r10,
// G/header(g167:13052-13526), only mip 0/slice 0 observer-admitted path. Other
// call/unread word=nullopt with why; all destination formats otherwise modeled.
std::optional<ResolveCopyWords> DeriveResolveWords(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why = nullptr);

#if REREVVED_DEVELOPER
// Bit i is set when word i (kResolveWordNames order) differs.
unsigned ResolveWordMismatches(const ResolveCopyWords& derived, const ResolveCopyWords& observed) noexcept;

// The cvar native_resolve_derive_check.
bool ResolveDeriveCheckRequested() noexcept;
#else
// The check cvars exist only in the developer build.
constexpr bool ResolveDeriveCheckRequested() noexcept
{
    return false;
}
#endif

// The guest words a Resolve call changes besides the copy words, as the
// generated body (g167:13052-15160) leaves them at its return 0x8269F360.
inline constexpr std::size_t kResolveDirtyWords = 3; // the 64-bit words G+0x10, G+0x18 and G+0x20

// Bits of the byte G+0x2ABD (a command-buffer allocation failed; set by a
// depth clear) and of G+0x2ABE (cleared by every call).
inline constexpr std::uint8_t kResolveAllocFailedBit = 0x20, kResolveDepthClearBit = 0x04, kResolveStateClearedBit = 0x08;
// The dirty bits G+0x10 keeps through every flush group of the body.
inline constexpr std::uint64_t kResolveProtectedDirty = 0x1E0000;

// The entry state beyond r3..r10 that the clear blocks read: the caller's
// stack words at r1+92 (the stencil value) and r1+100 (the resolve parameter
// block pointer), and the depth value in f1 (g167:13519, 13539-13545).
struct ResolveClearArgs
{
    std::uint32_t sp    = 0; // r1 at the entry
    double        depth = 0; // f1 at the entry
};

// Clear blocks 0x8269DF00/0x8269E448(flags 0x200/0x100) store G+0x2A28/+0x2A2C
// and +0x2A30/+0x2A34 after copy hook 0x8269E8A0; that hook sees previous-call
// words.
inline constexpr std::uint32_t kResolveClearShadow    = 0x2A28; // G+0x2A28..0x2A34
inline constexpr std::size_t   kResolveClearWordCount = 4;

// The image constants and tables the clear blocks read. The host reads them
// from guest memory like the format table.
inline constexpr std::uint32_t kResolveClearScale11   = 0x8208EB90; // float: the depth scale of the 11-bit control field
inline constexpr std::uint32_t kResolveClearScale24   = 0x8208EB94; // float: the depth scale of the 24-bit depth
inline constexpr std::uint32_t kResolveClearDefault   = 0x8208EBE0; // 16 bytes: the clear color for a null r10
inline constexpr std::uint32_t kResolveClearFormatMap = 0x8208EB50; // bytes by the shadow word's format field
inline constexpr std::uint32_t kResolveClearAlphaBits = 0x8208EBD0; // bytes by color format: the alpha channel width
inline constexpr std::uint32_t kResolveClearColorBits = 0x8208EBD4; // bytes by color format: the red, green and blue channel width
inline constexpr std::uint32_t kResolveClearOne       = 0x820068A8; // float subtracted from each channel's range

struct ResolveEffects
{
    ResolveCopyWords words; // the words the body stores at G+0x2A18..0x2A24 before its copy hook
    bool             large       = false;
    std::uint32_t    pitchHeight = 0;     // G+0x2A44 after a large call: pitch times height, 32 bits
    bool             depthClear  = false; // flags bit 0x200
    bool             colorClear  = false; // flags bit 0x100
    // The words the clear blocks store: G+0x2A28 and G+0x2A2C for flags 0x200,
    // G+0x2A30 and G+0x2A34 for flags 0x100. `clearRefused` names the reason a
    // 0x100 color is not modeled (the unmodeled color format or a NaN), else null.
    std::array<std::uint32_t, 2> depthWords{}, colorWords{};
    const char*                  clearRefused = nullptr;
    // A clear input (the color vector, the parameter block, a table) lies in a word
    // the body stores before the blocks read it, so the derived words are not Original's.
    bool clearOverlap = false;
    // OR masks into G+0x10, G+0x18 and G+0x20 that survive to the return: those
    // set before the allocator call at 0x8269F004 and those after it, which an
    // allocation failure skips together with the exit stores.
    std::array<std::uint64_t, kResolveDirtyWords> setBeforeAlloc{};
    std::array<std::uint64_t, kResolveDirtyWords> setAfterAlloc{};
};

// Resolve derivation returns nullopt with why for unread words or missing clear
// input.
std::optional<ResolveEffects> DeriveResolveEffects(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why = nullptr, const ResolveClearArgs* clear = nullptr);

// Resolve tiling/predication field and branch proof: only ring/cursor stores
//   and the null-pool bracket branch differ.
// Field/function          | Guest fact / source
// Tiling byte             | G+0x2ABC >> 24; predicate G+0x31AC; bracket
//   G+0x33B0.
// BeginTiling             | Stores bracket from G+0x33B8 (g123:12809-12817);
//   EndTiling clears it (g12:12984-12988).
// 0x8269CE90             | Sets byte bit 0x20 before BeginTiling
//   (g76:13421-13447); 0x8269D320 clears it (g70:13136-13151).
// SetPredication          | Stores G+0x31AC and mask G+0x31A4 (g171:12584,
//   12708, 12717, 12721); 0x8269D320 clears mask (g70:13146).
// 0x8250AC90 per-tile     | Resolves inside bracket with tile-mask predicate
//   (g205:8896, 9007, 9264, 9309).
// Tile-record branch      | Byte bit 0x10, or bit 0x20 with each G+0x3098 + 4i
//   equal to G+0x31B8 + 4i snapshot or zero.
// 0x826AC8C8 branch       | Also requires byte G+0x2ABF bit 0x20.
// Branch ranges           | g167:13868-13965, g202:13082-13186,
//   g153:12670-12769.
// Extra ring words        | g167:13966-14126, g202:13207-13297,
//   g153:12839-12994; final header ORs predicate (g167:14825-14833).
// Bracket read            | Allocator only (g93:12840-12841, g185:12955-13124).
// Other predicate loads   | Ring words G+0x31A4/G+0x31A8 only
//   (g167:14110-14120, g153:13067-13079, g202:13353-13363, 13580-13590).
// Untested changed bit    | SetPredication changes tiling byte bit 0 only;
//   closure tests 0x10/0x20/0x40 and flush mask 0xFFFFFF80 (g58:12963,
//   g202:12906, 12961).
// Other body/0x826ACC60/allocator stores | No tiling-byte/predicate/G+0x31A4
//   reads or stores except those above.
// Exit equivalence        | Predicated call in an open bracket has the same
//   exit state as the same call with predicate clear.
enum class ResolveTiling : std::uint8_t
{
    None,              // byte bits 0x30 clear, predicate and bracket word clear: the untiled body
    Bracket,           // byte bits 0x30 exactly 0x20, bracket word set, predicate clear: modeled
    PredicatedBracket, // the same with the predicate set: the per-tile Resolves of 0x8250AC90, modeled as the bracket form
    Byte,              // byte bit 0x10 set: its setter is not known
    Predicate,         // the predicate is set without an open bracket (byte bit 0x20 and the bracket word together): a shape not observed
    State,             // byte bit 0x20 without the bracket word, or the bracket word without byte bit 0x20
};

// The bracket form of a tiled call, with or without the per-tile predicate.
constexpr bool ResolveBracketForm(ResolveTiling tiling) noexcept
{
    return tiling == ResolveTiling::Bracket || tiling == ResolveTiling::PredicatedBracket;
}

// The tests run in this order: the byte, the predicate, then the agreement of
// the byte with the bracket word.
constexpr ResolveTiling ClassifyResolveTiling(std::uint8_t tilingByte, std::uint32_t predicate, std::uint32_t bracket) noexcept
{
    if (tilingByte & 0x10)
        return ResolveTiling::Byte;
    const bool open = (tilingByte & 0x20) != 0;
    if (predicate)
        return open && bracket != 0 ? ResolveTiling::PredicatedBracket : ResolveTiling::Predicate;
    if (open != (bracket != 0))
        return ResolveTiling::State;
    return open ? ResolveTiling::Bracket : ResolveTiling::None;
}

// The shape of a call of the predicated bracket form, for the census counters:
// what is known from the entry words alone, whatever else refuses the call.
struct ResolveShape
{
    bool          predicated = false; // ResolveTiling::PredicatedBracket
    bool          clear      = false; // flags 0x100 or 0x200
    std::uint32_t tiles      = 0;     // the tile count G+0x31CC
};

#if REREVVED_DEVELOPER
// The tile count buckets of the counters: 2, 3, 4 and any other count.
inline constexpr std::size_t kResolveTileBuckets = 4;

constexpr std::size_t ResolveTileBucket(std::uint32_t tiles) noexcept
{
    return tiles >= 2 && tiles <= 4 ? tiles - 2 : 3;
}

struct ResolveShapeCounts
{
    std::uint64_t                                  predicated = 0, predicatedClear = 0;
    std::array<std::uint64_t, kResolveTileBuckets> tiles{}; // of the predicated calls, by ResolveTileBucket
};

// " <prefix>predicated=N <prefix>predicated_clear=N <prefix>predicated_tiles_<2|3|4|other>=N".
std::string FormatResolveShapeCounts(const ResolveShapeCounts& counts, const char* prefix);
#endif

// The words the check compares, read at the entry and again at the return.
struct ResolveWatch
{
    std::array<std::uint32_t, 4>                  copy{};   // G+0x2A18..0x2A24
    std::uint32_t                                 size = 0; // G+0x2A44
    std::array<std::uint64_t, kResolveDirtyWords> dirty{};
    std::uint8_t                                  allocByte = 0, stateByte = 0; // G+0x2ABD, G+0x2ABE
    std::uint32_t                                 fence = 0;                    // G+0x2A9C
    std::uint32_t                                 dest8 = 0, dest12 = 0;        // header+8, header+12
    // The ring and the side-buffer pool the body's ring-space checks and its
    // allocator call 0x8269B818 read and move (see ResolveRingModel).
    std::array<std::uint32_t, kResolveClearWordCount> clear{};                              // G+0x2A28..0x2A34: the clear blocks' shadow words
    std::uint32_t                                     cursor = 0, limit34 = 0, limit38 = 0; // G+0x30, G+0x34, G+0x38
    std::uint32_t                                     poolPos = 0, poolUsed = 0;            // G+0x3A44, G+0x3A54
    std::uint32_t                                     wrapPos = 0;                          // G+0x3A4C: the null pool allocator's wrap position
    // Model reads G+0x3A58/G+0x31CC/G+0x34CC/G+0x33B0/G+0x31AC and tiling byte
    // G+0x2ABC >> 24; these are not compared.
    std::uint32_t poolThreshold = 0, targetCount = 0, poolObject = 0, poolBase = 0, poolSize = 0, bracket = 0, predicate = 0;
    std::uint8_t  tilingByte = 0;
    // Null allocator 0x8269B618/0x8269B158 reads G+0x3A3C/G+0x3A50/G+0x3A48,
    // G+0x33B4, and [[G+0x2A90] + 4].
    std::uint32_t sideEnd = 0, wrapLimit = 0, segment = 0, bracketNext = 0, progress = 0;

    ResolveTiling Tiling() const noexcept
    {
        return ClassifyResolveTiling(tilingByte, predicate, bracket);
    }
};

// The span of the device the watch reads: G+0x3A58, the pool threshold, is its
// highest word. The pool object and the progress block are read through their
// own addresses.
inline constexpr std::uint32_t kResolveDeviceSpan = 0x3A5C;

// The memory the skipped Resolve Original is checked against (stack_guard.h):
//   the stack reach [limit, r1 + 104), where the body reads the stencil value
//   at the entry r1+92 and the resolve parameter block pointer at r1+100 (lwz
//   460 and 468(r1) under the 368-byte frame, g167:13519 and 13539), the call's
//   parameter words; and what the model reads or writes, from the registers
//   r3..r10: device     [G, G + kResolveDeviceSpan): the copy, dirty and clear
//   shadow words, ring cursor and limits, tiling, mode and bracket words,
//   render target shadows, pool words (G+0x3A58 the highest), all of which the
//   exit writes among surface    the source surface the flags name, G+0x30A8
//   for the depth slot else G+0x3098+4*slot: its multisample halfword word
//   +0x18 header     r6, the destination header, 52 bytes: the fetch constant
//   at +28..+51 is read, +8 and +12 are read and stored rectangle  r5, 8 bytes,
//   when not null point      r7, 8 bytes, or kResolveDefaultPoint when null
//   color      r10 rounded down to 16, 16 bytes, when not null (else the
//   static) parameters the clear parameter block the word r1+100 names, 12
//   bytes allocator  the pool object G+0x34CC names (+152, +156) and the
//   progress block G+0x2A90 names (+4) statics    the format table (64
//   halfwords), the point, the clear default color, scales, format map and
//   channel widths, words of the title image The pool's fixed buffer is neither
//   read nor written by the model. The entry function's own frame is 368 bytes
//   (stwu r1,-368(r1), g167:13049); a call with fewer bytes between r1 and the
//   stack limit is refused.
inline constexpr StackFootprint             kResolveStackFootprint{ 104, kResolveDeviceSpan, 368 };
inline constexpr std::array<StackObject, 8> kResolveStackStatics{ { { kResolveFormatTable, 128 },
                                                                    { kResolveDefaultPoint, 8 },
                                                                    { kResolveClearScale11, 4 },
                                                                    { kResolveClearScale24, 4 },
                                                                    { kResolveClearDefault, 16 },
                                                                    { kResolveClearFormatMap, 16 },
                                                                    { kResolveClearAlphaBits, 8 },
                                                                    { kResolveClearOne, 4 } } };

// Resolve stack overlap includes device/object words or unknown reach; the
// consume verdict tests it before guest reads.
bool ResolveStackOverlaps(const GuestWordReader& read, const StackFrame& frame, const std::array<std::uint32_t, 8>& r);

// False when a word is unreadable or an address would wrap.
bool ReadResolveWatch(const GuestWordReader& read, std::uint32_t g, std::uint32_t header, ResolveWatch& out);

// The dirty bits the body's flush groups clear from G+0x10, G+0x18 and G+0x20
//   (g167:13644-13867). Each group tests its own bits and clears them, so a
//   group is the same as an unconditional clear of its bits whatever G+0x28
//   holds. The first call of the G+0x10 group, 0x826AC8C8, returns its dirty
//   argument r4 in r24 with bit 8 cleared when it took its flush branch and
//   bits 37 and 57 cleared when it emitted their blocks (g202:13407-13410,
//   13436-13439, 13471-13474, 13598-13599), and the body stores it back to
//   G+0x10 (g167:13664); those three bits lie inside the clear below, which the
//   groups that follow apply to the stored word, so the clear holds with or
//   without the call: G+0x10 bits 0..16 and 21..57 (four groups); bits 17..20
//   and 58..63 stay G+0x18 bits 34..54 G+0x20 bits 0..37 The flush callees
//   0x826ACC60 and 0x8268D3A0 write ring words and G+0x30.
inline constexpr std::array<std::uint64_t, kResolveDirtyWords> kResolveFlushClear{ 0x03FFFFFFFFE1FFFFULL, 0x007FFFFC00000000ULL, 0x0000003FFFFFFFFFULL };

// Every watched word at the return of a call whose exit is exact: the call the
// consume skips. Order of kResolveExitWordNames.
struct ResolveExitState
{
    std::array<std::uint32_t, 4>                      copy{};   // G+0x2A18..0x2A24: the exit stores zero the control and info words
    std::uint32_t                                     size = 0; // G+0x2A44
    std::array<std::uint64_t, kResolveDirtyWords>     dirty{};
    std::uint8_t                                      allocByte = 0, stateByte = 0; // G+0x2ABD, G+0x2ABE
    std::uint32_t                                     dest8 = 0, dest12 = 0;        // header+8, header+12
    std::uint32_t                                     limit34 = 0, limit38 = 0;     // G+0x34, G+0x38: the side buffer pool shrinks them
    std::uint32_t                                     poolPos = 0, poolUsed = 0;    // G+0x3A44, G+0x3A54
    std::uint32_t                                     wrapPos = 0;                  // G+0x3A4C
    std::array<std::uint32_t, kResolveClearWordCount> clear{};                      // G+0x2A28..0x2A34
};

#if REREVVED_DEVELOPER
inline constexpr std::size_t kResolveExitWordCount = 21;

inline constexpr std::array<const char*, kResolveExitWordCount> kResolveExitWordNames{
    "copy_control",
    "dest_base",
    "dest_pitch",
    "dest_info",
    "size",
    "dirty_10",
    "dirty_18",
    "dirty_20",
    "alloc_byte",
    "state_byte",
    "dest_8",
    "dest_12",
    "limit_34",
    "limit_38",
    "pool_pos",
    "pool_used",
    "wrap_pos",
    "clear_control",
    "clear_depth",
    "clear_color",
    "clear_color_low",
};
#endif

// The ring space the body can use before Original would flush, and the side
//   buffer its allocator call hands out, from the entry state.
//  Every ring check of the body compares the cursor with G+0x38 and calls the
//   flush 0x8269C588 when it is past it (g167:13972, 14073, 14133, 14353,
//   14560, 14614, 14846, 14982; g202:13043, 13278; g153:12660, 12968), and
//   0x826ACC60 compares a run's end with G+0x34 and calls 0x826AC610 when it
//   does not fit below it (g71:12189-12203). The flush moves the fence G+0x2A9C
//   by two (g12:12903-12905 and the stub), so a call that could flush has no
//   exact exit. The cursor only grows through a call, so the body cannot flush
//   when the cursor plus every word it can emit stays at or below the soft
//   limit G+0x38 and below the hard limit G+0x34, both as the side buffer
//   allocation leaves them. The words it can emit, counting every optional
//   block and the per-tile blocks of a tiled call (ResolveTiling), with n the
//   tile count G+0x31CC: 0x826AC8C8 (twice)      32 + 5n each
//   (g202:13191-13594) 0x8268D3A0 (twice)      12 + 7n each (g153) 0x826ACC60
//   flush groups popcount + runs of each group's dirty bits (g71:12148-12330),
//   the G+0x10 bits 0..11, 12..16, 42..57 and 21..41, G+0x18 bits 34..54 and
//   the G+0x20 bits 0..37 with the 0x3C00, the 4 (large form) and the 0x300 and
//   0xC0 of the clear blocks the body ORs in (g167:13476, 13530-13534,
//   g50:12537, g209:13162) before the flush groups the per-tile block      6 +
//   5n (g167:13966-14126) the straight-line emits 6 + 29 + 14 + 2 + 13 + 4 + 17
//   = 85 (g167:14127-15100) The allocator 0x8269B818 reads G+0x34CC
//   (g93:12786-12824): a pool object with no fixed buffer (+152 zero) is
//   0x826A6AB8, an indirect allocator the host does not model (alloc_path). A
//   null pool object is 0x8269B618, modeled by
// ModelNullPoolAllocator: it moves neither G+0x34 nor G+0x38. With a fixed
// buffer, 0x826A6848 takes the 24 bytes from its top, G+0x3A44 or +152 plus the
//   aligned +156, at (top - 24) & ~3, fails when that is below the cursor plus
//   164, and otherwise stores it to G+0x3A44 and lowers G+0x34 and G+0x38 by
//   (top - that) & ~3 (g100:13277-13308). 0x8269B818 then adds 24 to G+0x3A54
//   and, past G+0x3A58 with the bracket G+0x33B0 clear, moves G+0x34 to the
//   cursor plus 160 and G+0x38 to the cursor (g93:12826-12865): the 17 and more
//   words that follow make the next check flush.
enum class ResolveExitRefusal : std::uint8_t
{
    None,
    Fence,           // G+0x2A9C is zero: the list path
    AllocMarker,     // G+0x2ABD & 0x20 at the entry
    Tiling,          // the tiling byte has bit 0x10 (ResolveTiling::Byte)
    TilingPredicate, // the predicate G+0x31AC is set outside an open bracket (ResolveTiling::Predicate)
    TilingState,     // the tiling byte and the bracket word G+0x33B0 disagree (ResolveTiling::State)
    ClearOverlap,    // a clear input overlaps a guest word the body stores before the blocks (ResolveEffects::clearOverlap)
    ClearColor,      // flags 0x100 with a color the clear block's words are not modeled for (ResolveEffects::clearRefused)
    AllocPath,       // a pool object without a fixed buffer: the indirect allocator 0x826A6AB8
    RingSpace,       // Original could flush: the ring words may pass the limits, or the side buffer count passes its threshold
    AllocCapacity,   // the allocator would fail: the fixed buffer cannot hold the side buffer below the ring, or the null pool allocator has no room
    AllocWait,       // the null pool allocator would wait in 0x8269B158 for the GPU to leave the segment
    // ResolveStackOverlaps includes unknown or stack-held model words/objects.
    // DeriveResolveExitState has no stack test; consume tests it and
    // CompareResolveEffects uses the probe's result.
    StackOverlap,
};
inline constexpr std::size_t                                       kResolveExitRefusalCount = static_cast<std::size_t>(ResolveExitRefusal::StackOverlap) + 1;
inline constexpr std::array<const char*, kResolveExitRefusalCount> kResolveExitRefusalNames{
    "none",
    "fence",
    "alloc_marker",
    "tiling",
    "tiling_predicate",
    "tiling_state",
    "clear_overlap",
    "clear_color",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
    "stack_overlap",
};

// What the null pool allocator 0x8269B618 does for the 24-byte side buffer of
//   the body (0x8269B818 calls it with r4 = 1, r6 = 4; g185:12899-13183), and
//   whether its wait 0x8269B158 (g206:12421-12513) would spin. All arithmetic
//   is 32 bits and every compare unsigned except the bracket's, which test the
//   32-bit difference against -1 and 0 as signed words. start   G+0x3A44, or
//   G+0x34 + 60 while it is zero forward buffer (start + 3) & ~3, end buffer +
//   24; taken when end <= G+0x3A3C wrap    otherwise buffer (G+0x3A4C + 3) &
//   ~3, end buffer + 24; fails when end > G+0x3A50 bracket (G+0x33B0 nonzero)
//   forward: fails when G+0x33B4 - G+0x3A48 is -1, or is 0 with G+0x33B0 < end;
//   wrap: the same with the difference less 1 store   forward: G+0x3A44 = end;
//   wrap: G+0x3A4C = end (G+0x3A48 stays) wait    with n = G+0x3A48 (+1 on a
//   wrap) and v the progress word, spins unless (n - v) & 3 is 0, or is 1 with
//   end <= v & ~3 A buffer address of zero is a failure to the caller, whatever
//   the stores did.
struct ResolveNullPoolAlloc
{
    bool          ok    = false; // a buffer is returned
    bool          wrap  = false; // taken at the wrap position G+0x3A4C rather than G+0x3A44
    bool          waits = false; // 0x8269B158 would spin
    std::uint32_t end   = 0;     // the value stored at G+0x3A4C (wrap) or G+0x3A44
};

// Take bytes/alignment:0x8269B818 wordcount*4/r5(g93:12757-12790); Resolve24/4,
// Clear 0x826A1800 strips 84/4(g96:12915-12925). For consecutive modeled takes,
// store end into watch.poolPos or wrapPos before next.
ResolveNullPoolAlloc ModelNullPoolAllocator(const ResolveWatch& entry, std::uint32_t bytes = 24, std::uint32_t alignment = 4) noexcept;

// clearDirty G+0x20 is 0x300 for flags 0x200 and 0xC0 for 0x100; ring words are
// UINT64_MAX when tile bound exceeds loops.
std::uint64_t ResolveMaxRingWords(const ResolveWatch& entry, std::uint64_t clearDirty = 0) noexcept;

// The exact values of the watched words at the return, from the derived
// effects and the watched words at the entry:
//   copy      zero, base, pitch, zero (g167:15083-15086)
//   size      pitch times height on the large form, else as at the entry
//   dirty     (entry & ~kResolveFlushClear) | the bits set before and after the
//             allocator call (the flush clears precede every set)
//   allocByte the entry with 0x04 set for flags 0x200 (g167:14299-14311)
//   clear     the entry words, with G+0x2A28 and G+0x2A2C replaced for flags
//             0x200 and G+0x2A30 and G+0x2A34 for flags 0x100 (the blocks
//             0x8269DF00 and 0x8269E448 store nothing else outside their
//             frames and G+0x20 bits the flush groups clear again)
//   stateByte the entry with 0x08 cleared (g167:14185-14195)
//   dest8/12  both the ENTRY fence (g167:14928, 15112, 15114): a call that
//             cannot flush leaves G+0x2A9C alone
//   limit/pool the side buffer allocation's moves, from the entry words: a
//             fixed pool lowers G+0x34 and G+0x38 and stores G+0x3A44; a null
//             pool object stores G+0x3A44 or G+0x3A4C and leaves both limits;
//             either adds 24 to G+0x3A54
// It models only the call that takes the header path, with the body's
// allocator call succeeding and Original unable to flush or wait. It refuses
// (nullopt, `refusal` and `why` set) a zero fence (the
// list path, which never stores dest+12), the allocation failure marker set
// at the entry, a tiled device other than the bracket form of ResolveTiling,
// flags 0x100 with a color the block's words are not modeled for, a
// pool object without a fixed buffer, a call whose ring words may pass a limit,
// an allocation that may fail and a null pool allocation that would wait. The
// bracket form adds only ring words, which ResolveMaxRingWords already counts,
// and the allocator's bracket terms. The order of those tests is the order of
// ResolveExitRefusal.
std::optional<ResolveExitState> DeriveResolveExitState(const ResolveEffects& effects, const ResolveWatch& entry, ResolveExitRefusal* refusal = nullptr, std::string* why = nullptr);
// Reads the watched words at the entry registers `r` and derives the effects, then
// the exit state. Refuses as DeriveResolveEffects does, and for an unreadable
// watched word.
std::optional<ResolveExitState> DeriveResolveExitState(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why = nullptr, const ResolveClearArgs* clear = nullptr);

#if REREVVED_DEVELOPER
// Bit i is set when word i (kResolveExitWordNames order) of `exit` differs.
unsigned ResolveExitMismatches(const ResolveExitState& predicted, const ResolveWatch& exit) noexcept;
#endif

// One guest store of the exit state: a big-endian word, or the byte at G+0x2ABD
// or G+0x2ABE.
struct ResolveWrite
{
    std::uint32_t address = 0;
    std::uint32_t value   = 0;
    bool          byte    = false;

    friend bool operator==(const ResolveWrite&, const ResolveWrite&) = default;
};

// Store changed exit words in kResolveExitWordNames order. Dirty qwords are
// big-endian 32-bit halves, high first; store only changed halves.
std::vector<ResolveWrite> ResolveExitWrites(std::uint32_t g, std::uint32_t header, const ResolveWatch& entry, const ResolveExitState& exit);

#if REREVVED_DEVELOPER
enum class ResolveEffectField : unsigned
{
    CopyWords,
    Size,
    DirtySetMissing,
    DirtyExtra,
    Bytes,
    Dest,
};

inline constexpr std::size_t kResolveEffectFieldCount = 6;

inline constexpr std::array<const char*, kResolveEffectFieldCount> kResolveEffectFieldNames{
    "copy_words",
    "size",
    "dirty_set_missing",
    "dirty_extra",
    "bytes",
    "dest",
};

struct ResolveEffectResult
{
    unsigned                                      mismatch    = 0;     // bit i is ResolveEffectField i
    bool                                          allocFailed = false; // the marker G+0x2ABD & 0x20 is set at the return, whichever allocation set it
    bool                                          mainFailed  = false; // the allocator call 0x8269B818 of the body failed
    bool                                          listPath = false, cleared1E0000 = false;
    unsigned                                      clearedBits = 0;
    std::array<std::uint64_t, kResolveDirtyWords> missing{}, extra{}; // predicted bits clear at the return; bits set that were clear at entry and not predicted
    std::array<std::uint32_t, 4>                  expectCopy{};       // G+0x2A18..0x2A24 as predicted
    std::uint32_t                                 expectSize = 0;     // G+0x2A44 as predicted
    // Exit comparison covers every modeled call; marker, list, or fence changes
    // mismatch prediction, while refusal names exactRefusal.
    bool exactModeled    = false;
    bool exactTiled      = false; // modeled, and the entry was the bracket form of ResolveTiling, predicated or not
    bool exactPredicated = false; // modeled, and the entry was ResolveTiling::PredicatedBracket (also exactTiled)
    // What the entry was, whether or not the exit state models the call, so a
    // refusal earlier in the order does not hide the shape (see ResolveShape).
    ResolveShape       shape;
    bool               exactClear    = false; // modeled, and the flags had 0x200 or 0x100
    unsigned           exactMismatch = 0;     // bit i is word i of kResolveExitWordNames
    ResolveExitRefusal exactRefusal  = ResolveExitRefusal::None;
    ResolveExitState   exact;
};

// Resolve comparison fact table:
// Flush: an entry bit set at entry and clear at return is a counted flush and
//   never a mismatch.
// Marker: the body allocator's failure is not established by the marker alone;
//   list allocation 0x826A6958 can set it after body success.
// Allocation failure: post-allocator bits, exit stores, and header words are
//   not predicted; G+0x2A18 and G+0x2A24 retain their copy words.
// Fence timing: G+0x2A9C advances by two whenever the body waits for ring
//   space, from its first read stored to +8 through reload g167:15104, and
//   again after reload; the return snapshot is not the reload.
// Sequence: S contains fence values from the entry read to the return read in
//   steps of two, wrapping.
// Nonzero reload: it stores itself to +8 and +12 and goes straight to the
//   return hook, so both equal the nonzero return fence.
// Zero reload: the list path requires zero in S, leaves +12 as at entry, and
//   has +8 at an S value at or before zero.
// Mismatch: every other header pair is a Dest mismatch.
ResolveEffectResult CompareResolveEffects(const ResolveEffects& predicted, const ResolveWatch& entry, const ResolveWatch& exit) noexcept;

// The cvar native_resolve_effect_check.
bool ResolveEffectCheckRequested() noexcept;
#else
// The check cvars exist only in the developer build.
constexpr bool ResolveEffectCheckRequested() noexcept
{
    return false;
}
#endif

// Resolve frame subtracts 368 from entry r1 at copy 0x8269E8A0 and return
// 0x8269F360 (g167:13047-13049, g167:15161-15163).
inline constexpr std::uint32_t kResolveFrameSize = 368;

// One Resolve call between its entry and its return hook. The stack keeps the
// frames of the Resolves in flight, so a consume can refuse a Resolve nested in
// another; the developer build also keeps the comparison state of the check
// cvars.
struct ResolveProbe
{
    bool          admitted   = false; // the entry observer admitted it; otherwise a placeholder that keeps its frame
    bool          deriveCopy = true;  // native_resolve_derive_check was on at the entry
    bool          effects    = false; // native_resolve_effect_check was on at the entry and the call was admitted
    std::uint32_t frame      = 0;     // r1 inside the body
#if REREVVED_DEVELOPER
    std::optional<ResolveEffects>   predicted;
    ResolveWatch                    entryWatch;
    std::string                     effectsWhy;           // the unmodeled reason when `predicted` is empty
    bool                            hookRead     = false; // the copy hook ran and saved `hookClear`
    std::uint32_t                   g            = 0;
    bool                            stackOverlap = false; // ResolveStackOverlaps at the entry, tested before any guest word of the probe is read
    std::array<std::uint32_t, 8>    entry{};
    std::optional<ResolveCopyWords> derived;
    std::string                     why;         // the unmodeled reason when `derived` is empty
    std::array<std::uint32_t, 3>    hookClear{}; // G+0x2A2C, G+0x2A30, G+0x2A34 at 0x8269E8A0
#endif
};

// How many dropped probes belonged to each check.
struct ResolveDropCounts
{
    unsigned derive = 0, effects = 0;
};

// Resolve probes are outermost-first; reentry or missing return drops stale
// frames above the matching call.
class ResolveProbeStack
{
public:
    static constexpr std::size_t kDepth = 4;

    // Probe push drops stale lower frames and reports deriveCopy/effects drops;
    // full stack refuses to keep the new probe.
    unsigned Push(std::uint32_t entrySp, ResolveProbe probe, ResolveDropCounts* drops = nullptr);
    // The topmost probe with this frame, with everything above it dropped and
    // counted in `dropped`; nullptr, with nothing changed, when none matches.
    ResolveProbe* Find(std::uint32_t frame, unsigned& dropped, ResolveDropCounts* drops = nullptr);
    // As Find, then removes the probe too.
    std::optional<ResolveProbe> Pop(std::uint32_t frame, unsigned& dropped, ResolveDropCounts* drops = nullptr);

    std::size_t Size() const noexcept
    {
        return count_;
    }

    void Clear() noexcept
    {
        count_ = 0;
    }

private:
    std::array<ResolveProbe, kDepth> slots_;
    std::size_t                      count_ = 0;
};

#if REREVVED_DEVELOPER
// Counts one comparison and returns its mismatch mask. A nullopt `derived` is
// an unmodeled call and counts unsupported; otherwise a zero mask counts ok
// and each differing word counts under its own name.
unsigned NoteResolveDerive(const std::optional<ResolveCopyWords>& derived, const ResolveCopyWords& observed) noexcept;
// Counts probes dropped unpaired: a call that never reached its hooks, a full
// probe stack, or a copy hook whose device is not the probe's.
void NoteResolveUnpaired(unsigned count) noexcept;
// Counts one comparison of the clear words G+0x2A2C..0x2A34 at the hook with
// their values at the Resolve return.
void NoteResolveClearWords(bool stale) noexcept;

struct ResolveDeriveCounts
{
    std::uint64_t                                ok = 0, unsupported = 0, unpaired = 0;
    std::array<std::uint64_t, kResolveWordCount> mismatch{};
    std::uint64_t                                clearChecked = 0, clearStale = 0;
};

// Returns the counts since the previous take and resets them.
ResolveDeriveCounts TakeResolveDeriveCounts() noexcept;
// Resolve diagnostics report copy-control/destination mismatches, clear checks,
// staleness, unsupported, and unpaired calls.
std::string FormatResolveDeriveCounts(const ResolveDeriveCounts& counts);

// Counts one effect comparison: ok when no field differs, otherwise under each
// differing field; flush clears, an allocation failure and the list path are
// counted whether or not a field differs.
void NoteResolveEffect(const ResolveEffectResult& result) noexcept;
// Counts a call whose effects the derivation does not model.
void NoteResolveEffectUnsupported() noexcept;
// Counts a call whose stack reach ResolveStackOverlaps found in the call's memory at
// the entry, before any guest word of the probe was read: neither the effects nor
// the exact exit are derived, and the exact exit is skipped as stack_overlap.
void NoteResolveEffectStackOverlap() noexcept;
// Counts effect probes dropped unpaired.
void NoteResolveEffectUnpaired(unsigned count) noexcept;

struct ResolveEffectCounts
{
    std::uint64_t                                       ok = 0, unsupported = 0, unpaired = 0, dirtyCleared = 0, cleared1E0000 = 0, allocFailed = 0, listPath = 0;
    std::array<std::uint64_t, kResolveEffectFieldCount> mismatch{};
    // The exact comparison with DeriveResolveExitState: calls whose every word
    // equals it, calls it does not model (those the derivation refuses, by
    // reason, and the unsupported calls), and calls with a differing word.
    std::uint64_t exactOk = 0, exactSkipped = 0;
    // Of the modeled calls, those with the bracket form of ResolveTiling, and
    // those of them whose every word equals the exact exit.
    std::uint64_t exactTiled = 0, exactTiledOk = 0;
    // Likewise those with the predicate set (also in exactTiled).
    std::uint64_t exactPredicated = 0, exactPredicatedOk = 0;
    // Likewise those with clear flags, whose clear words the exit state derives.
    std::uint64_t exactClear = 0, exactClearOk = 0;
    // The calls of the predicated bracket form seen, modeled or not, with clear
    // flags, and by tile count (see ResolveShape).
    ResolveShapeCounts                                  shape;
    std::array<std::uint64_t, kResolveExitWordCount>    exactMismatch{};
    std::array<std::uint64_t, kResolveExitRefusalCount> exactRefused{}; // by ResolveExitRefusal; the None slot stays zero
};

// Returns the counts since the previous take and resets them.
ResolveEffectCounts TakeResolveEffectCounts() noexcept;

std::string FormatResolveEffectCounts(const ResolveEffectCounts& counts);
#endif

} // namespace rerevved::gpu::guest_d3d
