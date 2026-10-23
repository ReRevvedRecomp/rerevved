#pragma once

#include "guest_render_capture.h"
#include "resolve_derive.h"
#include "stack_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// native_consume_resolve skips 0x8269E520 only for exact modeled exit: replay
// renderer observation, 0x8269E8A0 ledger/assembler work and guest exit stores.
// Copy uses entry clear words; no architectural register changes.

// The Resolve entry.
inline constexpr std::uint32_t kResolveEntry = 0x8269E520;

constexpr bool ResolveConsumeEntry(std::uint32_t address) noexcept
{
    return address == kResolveEntry;
}

// Why a call ran its Original with the cvar on. The enumerators follow the
// order in which the verdict tests them.
enum class ResolveFallback : std::uint8_t
{
    // Refuse possible flush/alloc failure/wait: Original would move fence,
    // change path, keep words or block. Tiled bracket adds ring/cursor/bracket
    // allocator terms, no tile-list append; host work unchanged.
    Stubs,             // ring stubs are off or have not checked this device
    Nested,            // another Resolve of this thread is in flight
    Ledger,            // no ledger or no assembler is published
    EntryState,        // the entry observer's draft is invalid, not open, or another device's
    Diagnostics,       // a probe, capture, harvest or route trace needs the body
    StackOverlap,      // the thread's stack reach holds a device word or an object the model reads or writes, or is unknown
    Tiling,            // tiling byte bit 0x10, or the bracket form while native_consume_resolve_tiled is off
    TilingPredicate,   // the predicate G+0x31AC is set
    TilingState,       // the tiling byte bit 0x20 and the bracket word G+0x33B0 disagree
    DeriveUnsupported, // the derivation does not model the call
    Fence,             // G+0x2A9C is zero: the list path
    AllocMarker,       // G+0x2ABD & 0x20 at the entry: the body's allocator returns at once
    ClearOverlap,      // a clear input overlaps a word the body stores before the clear blocks
    ClearColor,        // flags & 0x100 with a color format or value whose clear words the host does not derive
    AllocPath,         // the allocator path is not modeled: a pool object without a fixed buffer
    RingSpace,         // Original could flush: its ring words may pass a limit, or the side buffer count passes its threshold
    AllocCapacity,     // Original's allocator would fail: the fixed buffer cannot hold the side buffer below the ring, or the null pool allocator has no room
    AllocWait,         // the null pool allocator would wait in 0x8269B158 for the GPU
    Unreadable,        // a guest word could not be read
};
inline constexpr std::size_t                                         kResolveFallbackCount = static_cast<std::size_t>(ResolveFallback::Unreadable) + 1;
inline constexpr std::array<std::string_view, kResolveFallbackCount> kResolveFallbackNames{
    "stubs",
    "nested",
    "ledger",
    "entry_state",
    "diagnostics",
    "stack_overlap",
    "tiling",
    "tiling_predicate",
    "tiling_state",
    "derive_unsupported",
    "fence",
    "alloc_marker",
    "clear_overlap",
    "clear_color",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
    "unreadable",
};

// The cvar native_consume_resolve while ring stub services are published; a
// consume does nothing without them.
bool ResolveConsumeRequested() noexcept;
// The cvar native_consume_resolve_tiled, effective only with the one above.
bool ResolveTiledConsumeRequested() noexcept;

// What the caller knows of the host before the guest words are read.
struct ResolveConsumeState
{
    bool stubsReady  = false; // RingStubsReady(G)
    bool nested      = false; // a Resolve of this thread is in flight
    bool ledgerReady = false; // a ledger and an assembler are published
    bool entryState  = false; // the entry observer opened a draft for this device that the ledger still awaits
    bool diagnostics = false; // something needs the body or its midasm hooks
    bool tiled       = false; // native_consume_resolve_tiled: the bracket form of a tiled call may be consumed, with or without the per-tile predicate
    // r13 (the guest KPCR) and r1 at the entry; the stack is refused without them.
    std::uint32_t pcr = 0, sp = 0;
};

// Everything a consumed call replays.
struct ResolvePlan
{
    // Allocator 0x8269B818 stores G+0x3A44 (null wrap G+0x3A4C), G+0x3A54 and
    // fixed-pool G+0x34/G+0x38. No host reads ring, G+0x30 or side bytes.
    ResolveEffects effects;
    ResolveWatch   watch; // the watched words at the entry
    // Clear 0x8269DF00/0x8269E448 flags update G+0x2A28..G+0x2A34 and the
    // G+0x2ABD depth bit.
    ResolveExitState exit;               // the exact words at the return
    bool             tiled      = false; // the bracket form of a tiled call, predicated or not
    bool             predicated = false; // the bracket form with the predicate set (also tiled)
};

// Empty fills plan. First failure:state, stack before reads, tiling, derive,
// fence, alloc marker, exit. Clear flags without clear=DeriveUnsupported; shape
// fills once tiling classifies, before later refusal.
std::optional<ResolveFallback> ResolveConsumeVerdict(const ResolveConsumeState& state, const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, ResolvePlan* plan = nullptr, const ResolveClearArgs* clear = nullptr, ResolveShape* shape = nullptr);

// The copy words and the clear words the copy hook would read for a planned call:
// the shadow words G+0x2A2C, G+0x2A30 and G+0x2A34 at the entry.
GuestResolveCopy ResolvePlanCopy(const ResolvePlan& plan);

#if REREVVED_DEVELOPER
// Counts the verdict of a call with the cvar on: a fallback by reason, or a
// consumed call.
void NoteResolveConsumed(bool tiled = false, bool predicated = false) noexcept;
void NoteResolveFallback(ResolveFallback reason) noexcept;
// Counts the shape of a call of the predicated bracket form, whatever its verdict.
void NoteResolveShape(const ResolveShape& shape) noexcept;

struct ResolveConsumeCounts
{
    std::uint64_t                                    consumed = 0, consumedTiled = 0, consumedPredicated = 0; // the tiled ones are also in consumed, the predicated ones in both
    ResolveShapeCounts                               shape;
    std::array<std::uint64_t, kResolveFallbackCount> fallback{};
};

// Returns the counts since the previous take and resets them.
ResolveConsumeCounts TakeResolveConsumeCounts() noexcept;

std::string FormatResolveConsumeCounts(const ResolveConsumeCounts& counts);
#else
// The consume counters exist only in the developer build.
inline void NoteResolveConsumed(bool = false, bool = false) noexcept
{
}

inline void NoteResolveFallback(ResolveFallback) noexcept
{
}

inline void NoteResolveShape(const ResolveShape&) noexcept
{
}
#endif

} // namespace rerevved::gpu::guest_d3d
