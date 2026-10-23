#pragma once

#include "clear_derive.h"
#include "guest_render_capture.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// Clear consume guest call/store map; exact exit proof is DeriveClearExitState.
// Path/word                    | Guest fact required by replay
// 0x826A2970 -> 0x826A22F8      | native_consume_clear skips untiled calls;
//   native_consume_clear_tiled admits tiled calls.
// 0x826A1D90                   | One RecordGuestClear per derived body call,
//   before its tile loop; DeriveClearCalls replaces live calls.
// Observation inputs           | ReadGuestClear supplies the device's tile
//   layout/predicate, whether tiled or untiled.
// Replayed dirty state         | G+0x10, G+0x18, G+0x20 and byte G+0x2ABD.
// Every strip                  | 0x826A1800 calls allocator 0x8269B818;
//   G+0x3A54 changes on every path, using the prior strip's stores.
// Pool object                  | Replay G+0x3A44 and ring limits G+0x34/G+0x38.
// Null pool (world)            | 0x8269B618 stores G+0x3A44 on forward take or
//   G+0x3A4C on wrap; neither ring limit moves.
// Omitted guest stores         | Flush-group ring words, blocks, strips,
//   side-buffer vertices and cursor G+0x30 have no host reader.
// Untouched by replay          | G+0x30, the pool object's other words and
//   G+0x3A48; threshold paths that move G+0x3A48 are refused.
// Registers                    | No wrapper caller reads the return value; r3
//   and all other registers keep entry values.
// ring_space                   | Refuse ring-limit/pool-threshold crossings
//   that can flush at 0x8269C588.
// alloc_capacity/alloc_path    | Refuse unproved side-buffer availability or
//   alternate/failure-marker paths.
// alloc_wait                   | Refuse null-pool GPU waits at 0x8269B158;
//   skipping them would change guest flush/kick cadence.
// Tiled admission              | ClearDeviceTiled loop or bracket G+0x33B0, in
//   any kClearShape combination.
// Selected tile                | One 0x826A1AF0 call; DeriveClearSideBuffers
//   derives its buffers.
// Tiled bracket                | Changes null-pool terms and removes the
//   pool-threshold flush; observation still runs once per body call.
// Tile block A                 | g88:13522-13630 is unmodeled and falls back as
//   tiled_block.

// The Clear wrapper.
inline constexpr std::uint32_t kClearEntryAddress = 0x826A2970;

constexpr bool ClearConsumeEntry(std::uint32_t address) noexcept
{
    return address == kClearEntryAddress;
}

// Why a call ran its Original with the cvar on. The enumerators follow the
// order in which the verdict tests them.
enum class ClearFallback : std::uint8_t
{
    Stubs,             // ring stubs are off or have not checked this device
    Nested,            // another Clear of this thread is in flight
    Ledger,            // no ledger or no assembler is published
    EntryState,        // the entry observer did not stack this call
    Diagnostics,       // a probe, capture, harvest or route trace needs the body or its midasm hooks
    StackOverlap,      // the thread's stack reach holds a device word or an object the model reads or writes, or is unknown
    Tiled,             // the device is tiled for 0x826A1D90 (tiling byte G+0x2ABC, saved surfaces) or the bracket G+0x33B0 is set, and native_consume_clear_tiled is off
    DeriveUnsupported, // the derivation does not model the call
    EarlyExit,         // the body leaves before its flush groups: nothing changes, nothing to replay
    TiledBlock,        // some call takes the tile block A, which is not modeled
    AllocMarker,       // G+0x2ABD & 0x20 at the entry: every allocation of Original returns at once
    Unreadable,        // a guest word could not be read
    AllocPath,         // the allocator path is not modeled: a pool object without a fixed buffer
    RingSpace,         // Original could flush: its ring words may pass a limit, or a take passes the pool threshold
    AllocCapacity,     // Original's allocator would fail: the fixed buffer cannot hold a side buffer below the ring, or the null pool allocator has no room
    AllocWait,         // the null pool allocator would wait in 0x8269B158 for the GPU to leave the segment
};
inline constexpr std::size_t                                       kClearFallbackCount = static_cast<std::size_t>(ClearFallback::AllocWait) + 1;
inline constexpr std::array<std::string_view, kClearFallbackCount> kClearFallbackNames{
    "stubs",
    "nested",
    "ledger",
    "entry_state",
    "diagnostics",
    "stack_overlap",
    "tiled",
    "derive_unsupported",
    "early_exit",
    "tiled_block",
    "alloc_marker",
    "unreadable",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
};

// The cvar native_consume_clear while ring stub services are published; a
// consume does nothing without them.
bool ClearConsumeRequested() noexcept;
// The cvar native_consume_clear_tiled, effective only with the one above.
bool ClearTiledConsumeRequested() noexcept;

// What the caller knows of the host before the guest words are read.
struct ClearConsumeState
{
    bool stubsReady  = false; // RingStubsReady(G)
    bool nested      = false; // a Clear of this thread other than this one is in flight
    bool ledgerReady = false; // a ledger and an assembler are published
    bool entryState  = false; // the entry observer stacked this call
    bool diagnostics = false; // something needs the body or its midasm hooks
    bool tiled       = false; // native_consume_clear_tiled: a tiled device may be consumed
};

// Everything a consumed call replays.
struct ClearPlan
{
    // Stack checks first prove device no-wrap, then KPCR/device reach, then
    // pointer word and object before reads. A frame overlapping those bytes
    // would change the modeled values.
    ClearEntry       entry;
    ClearCalls       calls;
    ClearEffects     effects;
    ClearSideBuffers sides;
    ClearWatch       watch; // the watched words at the entry
    ClearExitState   exit;  // the exact words at the exit
};

// Empty verdict fills plan, permits skip. First failure:state, stack before
// reads, tiled, derive, early exits, effects, blockA, alloc, side buffers,
// ring/allocator space. Unreadable reports at needed test.
std::optional<ClearFallback> ClearConsumeVerdict(const ClearConsumeState& state, const GuestWordReader& read, const ClearEntry& entry, ClearPlan* plan = nullptr);

// The registers r3..r10 the hook on 0x826A1D90 would see for call `index` of the
// plan, and the stencil and color words the body frame would hold for it.
std::array<std::uint32_t, 8> ClearPlanArguments(const ClearPlan& plan, std::size_t index);
GuestClearInputs             ClearPlanInputs(const ClearPlan& plan, std::size_t index);

#if REREVVED_DEVELOPER
// Counts the verdict of a call with the cvar on: a fallback by reason, or a
// consumed call.
// `shape` is the kClearShape bits of the consumed call (ClearExitState::shape).
void NoteClearConsumed(unsigned shape = 0) noexcept;
void NoteClearFallback(ClearFallback reason) noexcept;

struct ClearConsumeCounts
{
    std::uint64_t                                  consumed = 0, consumedTiled = 0; // the tiled ones are also in consumed
    std::array<std::uint64_t, kClearShapeCount>    shapes{};                        // the tiled ones, by kind (a shape can have several)
    std::array<std::uint64_t, kClearFallbackCount> fallback{};
};

// Returns the counts since the previous take and resets them.
ClearConsumeCounts TakeClearConsumeCounts() noexcept;
// " clear_consume_ok=N", " clear_consume_ok_tiled=N" and " clear_consume_tiled_<kind>=N"
// when nonzero, then " clear_consume_fallback_<reason>=N" for nonzero counts.
std::string FormatClearConsumeCounts(const ClearConsumeCounts& counts);
#else
// The consume counters exist only in the developer build.
inline void NoteClearConsumed(unsigned = 0) noexcept
{
}

inline void NoteClearFallback(ClearFallback) noexcept
{
}
#endif

} // namespace rerevved::gpu::guest_d3d
