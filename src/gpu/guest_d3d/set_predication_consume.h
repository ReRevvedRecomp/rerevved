#pragma once

#include "set_predication_derive.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// SetPredication 0x8269CD20 consume branch/store map
//   (native_consume_set_predication).
// Changed exit fields       | G+0x31AC, G+0x31A4, G+0x31A8, G+0x2AB4, G+0x2AB8
//   and byte G+0x2ABC; unchanged words are not stored.
// Tile-record bit           | Tested at g148:13542, g63:13607, g127:13621,
//   g151:13281; exit check compares with Original every call.
// Ordinary calls            | Skip ring words/cursor/frame/register stores;
//   G+0x30 stays at entry, 8 bytes behind Original per consume, delaying the
//   next ring flush.
// Ordinary caller reads     | None of r3 (new cursor), r8..r12, cr0 or cr6; all
//   registers retain entry state (set_predication_derive.h).
// Dead Original frame       | LR r1-8, r31 r1-16, old r1 r1-96
//   (g171:12565-12572); stack_guard.h rejects aliases with read/write device
//   words.
// Refusal order: stubs      | Device not checked by ring stubs.
// diagnostics               | A harvest, capture or route trace needs the body.
// preswap                   | Pre-swap LR lacks separate gate/trusted token.
// stack_overlap             | Unknown [limit, r1), or overlap with read/write
//   [G, G+0x31CC); tested before any guest read.
// unreadable                | Unreadable word or unwritable store target.
// ring_flush                | G+0x30 > G+0x38 calls 0x8269C588 in Original.
// Pre-swap stub reads       | Cursor for flushIfPastLimit at
//   0x826A419C/0x826A4280 and flip callback placement at 0x826A429C; an 8-byte
//   lag changes its flush boundary.
// Pre-swap admitted branch  | native_consume_set_predication_preswap plus
//   trusted r4=0: append two packets, G+0x30 += 8, return zero-extended cursor
//   in r3 (g226:12981-12991).
// Pre-swap store preflight  | Every store checked; reject packet aliases with
//   device, KPCR or whole stack reach.

constexpr bool SetPredicationConsumeEntry(std::uint32_t address) noexcept
{
    return address == kSetPredicationAddress;
}

// Why a call ran its Original with the cvar on. The enumerators follow the
// order in which the verdict tests them.
enum class SetPredicationFallback : std::uint8_t
{
    Stubs,        // ring stubs are off or have not checked this device
    Diagnostics,  // a route trace, harvest, scene capture or the exit check needs the body
    PreSwap,      // 0x826A4178 without the separate gate and trusted clear context
    StackOverlap, // the thread's stack reach holds a device word [G, G+0x31CC) the model reads or writes, or is unknown
    Unreadable,   // a guest word could not be read, a store target is not writable, or the device base is too high
    RingFlush,    // the cursor G+0x30 is above the soft limit G+0x38: Original calls 0x8269C588
};
inline constexpr std::size_t                                                kSetPredicationFallbackCount = static_cast<std::size_t>(SetPredicationFallback::RingFlush) + 1;
inline constexpr std::array<std::string_view, kSetPredicationFallbackCount> kSetPredicationFallbackNames{
    "stubs",
    "diagnostics",
    "preswap",
    "stack_overlap",
    "unreadable",
    "ring_flush",
};

// The cvar native_consume_set_predication while ring stub services are published;
// a consume does nothing without them.
bool SetPredicationConsumeRequested() noexcept;
bool SetPredicationPreSwapRequested() noexcept;
#if REREVVED_DEVELOPER
// The cvar native_set_predication_exit_check.
bool SetPredicationExitCheckRequested() noexcept;
// The pre-swap counters report only with the pre-swap consume or the exit check.
bool SetPredicationPreSwapReportingRequested() noexcept;
#else
// The check cvar and the counters exist only in the developer build.
constexpr bool SetPredicationExitCheckRequested() noexcept
{
    return false;
}

constexpr bool SetPredicationPreSwapReportingRequested() noexcept
{
    return false;
}
#endif

// One use, on this thread/context/device, around the host stub's census call.
class SetPredicationPreSwapScope
{
public:
    SetPredicationPreSwapScope(const PPCContext& ctx, std::uint32_t g) noexcept;
    ~SetPredicationPreSwapScope();
    SetPredicationPreSwapScope(const SetPredicationPreSwapScope&)            = delete;
    SetPredicationPreSwapScope& operator=(const SetPredicationPreSwapScope&) = delete;

private:
    friend class SetPredicationCallScope;
    const PPCContext*           context;
    std::uint32_t               device;
    bool                        claimed = false;
    bool                        paired  = false;
    SetPredicationPreSwapScope* previous;
};

// Installed before the census wrapper's observers; nested calls cannot reuse a token.
class SetPredicationCallScope
{
public:
    explicit SetPredicationCallScope(const PPCContext& ctx) noexcept;
    ~SetPredicationCallScope();
    SetPredicationCallScope(const SetPredicationCallScope&)            = delete;
    SetPredicationCallScope& operator=(const SetPredicationCallScope&) = delete;
    void                     Original() const noexcept;
    static bool              TrustedPreSwap() noexcept;

private:
    bool trusted = false;
#if REREVVED_DEVELOPER
    bool reporting = false;
#endif
    SetPredicationCallScope* previous;
};

// What the caller knows of the host before the guest words are read.
struct SetPredicationConsumeState
{
    bool          stubsReady     = false; // RingStubsReady(G)
    bool          diagnostics    = false; // something needs the body or its midasm hooks
    std::uint32_t caller         = 0;     // the link register at the entry
    std::uint32_t sp             = 0;     // r1 at the entry
    std::uint32_t pcr            = 0;     // r13 at the entry, the guest KPCR
    bool          preSwapEnabled = false;
    bool          trustedPreSwap = false;
#if REREVVED_DEVELOPER
    bool reportPreSwap = false; // count the pre-swap call
#endif
    // Optional for byte-addressed test memory; production supplies canonical backing ranges.
    std::function<std::optional<std::uint64_t>(std::uint32_t, std::uint32_t)> memoryIdentity;
};

// Everything a consumed call stores.
struct SetPredicationPlan
{
    SetPredicationWatch  watch;
    SetPredicationExit   exit;
    SetPredicationWrites writes;
};

// True when all `bytes` bytes at `address` are writable guest memory.
using SetPredicationWritable = std::function<bool(std::uint32_t address, std::uint32_t bytes)>;
// Stores one preflighted write, without throwing, as a guest `stw` or `stb` does.
using SetPredicationWriter = std::function<void(const SetPredicationWrite&)>;

// Empty when SetPredication(g, r4) may skip its Original; `plan` is then
// filled. The order of the tests is the order of SetPredicationFallback.
std::optional<SetPredicationFallback> SetPredicationConsumeVerdict(const SetPredicationConsumeState& state,
                                                                   const GuestWordReader&            read,
                                                                   const SetPredicationWritable&     writable,
                                                                   std::uint32_t                     g,
                                                                   std::uint32_t                     r4,
                                                                   SetPredicationPlan*               plan = nullptr);

// The verdict, the stores and the counts, over the given memory. True when the
// stores are done and the caller must skip Original.
bool ConsumeSetPredicationWith(const SetPredicationConsumeState& state,
                               const GuestWordReader&            read,
                               const SetPredicationWritable&     writable,
                               const SetPredicationWriter&       write,
                               std::uint32_t                     g,
                               std::uint32_t                     r4,
                               std::uint64_t*                    result = nullptr) noexcept;

// SetPredication consume returns false with cvar off or no published ring
// services; diagnostics keeps the body.
bool ConsumeSetPredication(PPCContext& ctx, bool stubsReady, bool diagnostics) noexcept;

#if REREVVED_DEVELOPER
struct SetPredicationConsumeCounts
{
    std::uint64_t                                           consumed = 0, consumedClear = 0; // the clear ones (r4 == 0) are also in consumed
    std::array<std::uint64_t, kSetPredicationFallbackCount> fallback{};
};

// Returns the counts since the previous take and resets them.
SetPredicationConsumeCounts TakeSetPredicationConsumeCounts() noexcept;
// " setpred_consume_ok=N setpred_consume_ok_clear=N", then
// " setpred_consume_fallback_<reason>=N" for nonzero counts.
std::string FormatSetPredicationConsumeCounts(const SetPredicationConsumeCounts& counts);

struct SetPredicationPreSwapCounts
{
    std::uint64_t                                           entered = 0, original = 0, consumed = 0;
    std::array<std::uint64_t, kSetPredicationFallbackCount> fallback{};
};

SetPredicationPreSwapCounts TakeSetPredicationPreSwapCounts() noexcept;
std::string                 FormatSetPredicationPreSwapCounts(const SetPredicationPreSwapCounts& counts);

// SetPredication exit probe compares Original after return and never changes
// execution.
struct SetPredicationProbeEntry
{
    std::uint32_t       g  = 0;
    std::uint32_t       r4 = 0;
    SetPredicationWatch before;
    SetPredicationExit  predicted;
    bool                preSwap = false;
};

// Entry absent if unreadable(count unreadable) or stack overlaps device/ring
// without flush(count unmodeled). Test stack before words beyond KPCR, ring
// after cursor read. Entry sp=r1, pcr=r13.
std::optional<SetPredicationProbeEntry> BeginSetPredicationProbe(const GuestWordReader& read,
                                                                 std::uint32_t          g,
                                                                 std::uint32_t          r4,
                                                                 std::uint32_t          sp,
                                                                 std::uint32_t          pcr,
                                                                 bool                   preSwap = false) noexcept;
// Reads the exit state after Original returned with r3 = `result`, compares it
// with the model and counts the call.
void EndSetPredicationProbe(const GuestWordReader& read, const SetPredicationProbeEntry& entry, std::uint32_t result) noexcept;

struct SetPredicationExitCounts
{
    std::uint64_t                                           ok = 0, flushed = 0, unreadable = 0, unmodeledStackOverlap = 0;
    std::array<std::uint64_t, kSetPredicationExitWordCount> mismatch{};

    struct PreSwap
    {
        std::uint64_t                                           ok = 0, exact = 0, flushed = 0, unreadable = 0, unmodeledStackOverlap = 0, unpaired = 0;
        std::array<std::uint64_t, kSetPredicationExitWordCount> mismatch{};
    } preSwap;
};

// Returns the counts since the previous take and resets them.
SetPredicationExitCounts TakeSetPredicationExitCounts() noexcept;
// " setpred_exit_ok=N setpred_exit_flushed=N setpred_exit_unreadable=N", then
// " setpred_exit_unmodeled_stack_overlap=N" when nonzero, then
// " setpred_exit_mismatch_<word>=N" for every compared word.
std::string FormatSetPredicationExitCounts(const SetPredicationExitCounts& counts);

class SetPredicationExitProbe
{
public:
    explicit SetPredicationExitProbe(const PPCContext& ctx) noexcept;
    ~SetPredicationExitProbe();

    SetPredicationExitProbe(const SetPredicationExitProbe&)            = delete;
    SetPredicationExitProbe& operator=(const SetPredicationExitProbe&) = delete;

    // True while the probe holds an entry state to compare.
    bool Armed() const noexcept
    {
        return static_cast<bool>(entry);
    }

private:
    std::unique_ptr<SetPredicationProbeEntry> entry;
    const PPCContext*                         context    = nullptr;
    int                                       exceptions = 0;
};
#endif

struct SetPredicationExitUnobserved
{
    void Original() const noexcept
    {
    }
};

template <std::uint32_t Address>
auto ObserveSetPredicationCall(const PPCContext& ctx) noexcept
{
    if constexpr (SetPredicationConsumeEntry(Address))
        return SetPredicationCallScope(ctx);
    else
        return SetPredicationExitUnobserved{};
}

// A probe only for the SetPredication entry, so the shared hook macro costs
// nothing for every other entry.
template <std::uint32_t Address>
auto ObserveSetPredicationExit([[maybe_unused]] const PPCContext& ctx) noexcept
{
#if REREVVED_DEVELOPER
    if constexpr (SetPredicationConsumeEntry(Address))
        return SetPredicationExitProbe(ctx);
    else
#endif
        return SetPredicationExitUnobserved{};
}

} // namespace rerevved::gpu::guest_d3d
