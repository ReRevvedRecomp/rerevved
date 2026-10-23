#pragma once

#include "indexed_buffer_consume.h"
#include "lowerer_derive.h"
#include "native_draw_dispatch.h"
#include "native_draw_hook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// Held lowerer consume guest call/store map; exit proof is
//   DeriveLowererExitState.
// Entry 0x826A3048             | native_consume_lowerer preserves
//   Original's dispatcher shadow, numbering and host draw.
// Replayed stores             | Allocator state; G+0x3484..0x3494
//   staging; two buffer pointers in caller out words; G+0x18 bit 0;
//   r3 = 0.
// Commit 0x826A2FF0            | ObserveNativeDrawCommit reads the
//   same guest buffers, dword counts and commit words.
// Out-buffer provenance       | g3:9292-9418 writes vertices/indices
//   through the out words before committing.
// Omitted stores              | Flush/draw ring words, G+0x30,
//   dirty-qword zeroing and a tiled root's tile record have no host
//   reader.
// Dirty/cursor invariant      | Dirty words remain a superset, so
//   next Original flushes skipped groups; G+0x30 stays at entry =
//   G+0x3484, including commit.
// Unmodeled paths             | Refuse flush, wait, failed allocation
//   or 0x826ADD88 (dirty 0x1E0000 bits, including those set by stride
//   changes).
// Sink 0x826A2B58              | Unconsumed: no direct caller was
//   observed to falsify its model; nested calls from 0x826A3000 use
//   user-pointer consume.
// Tiled admission             | native_consume_lowerer_tiled requires
//   byte 0x21, tiled IndexedBuffer bracket tripwires and room for a
//   tile record.
// Detailed derivation         | lowerer_derive.h.

// Lowerer skipped-body footprint (stack_guard.h); aliases refuse
//   before Original can overwrite modeled inputs.
// Reach/object                  | Guest fact
// Stack                         | [limit, r1 + 88); caller r1+84
//   holds vertex-out pointer (lwz 308(r1), 224-byte frame; g63:13775,
//   13798).
// Device                        | Through kLowererDeviceSpan, highest
//   ReadLowererWatch word G+0x3A58.
// LowererStackObjectsOverlap    | Pool object, progress block,
//   vertex/index out words (lowerer_derive.h).
// IndexedBufferStackObjectsOverlap | Shader/declaration objects, also
//   read by shared state gates.
// Consume stores                | Device exit words, both out words,
//   r3; omit ring words/cursor, dirty zeroing and tile record.
// Entry frame                   | 224 bytes, stwu r1,-224(r1)
//   (g63:13048); refuse if fewer bytes remain to stack limit.
inline constexpr StackFootprint kLowererStackFootprint{ 88, kLowererDeviceSpan, 224 };

// Why a root ran its Original with the cvar on. The enumerators follow the order in
// which the verdict tests them.
enum class LowererFallback : std::uint8_t
{
    Dispatcher,    // no live dispatcher observes the call
    Nested,        // the call is not a held root: nested in another observed draw, or the child call of an IndexedUp wrapper
    Stubs,         // ring stubs are off or have not checked this device
    StackOverlap,  // the thread's stack reach holds a device word or an object the model reads or writes, or is unknown
    Tiling,        // the tiling byte at G+0x2ABC admits the tile-record branch and is not the consumed 0x21, or the tiled cvar is off
    Unreadable,    // a guest word could not be read, or an out word or store target is not writable
    StateDirty,    // 0x826ADD88 would run: a dirty bit of 0x1E0000 or a stride change
    TiledState,    // a tiled root with a tripped tiled-state tripwire
    AllocMarker,   // G+0x2ABD & 0x20 at the entry
    TileGrowth,    // the tile list cannot take the record
    AllocPath,     // a pool object without a fixed buffer
    RingSpace,     // Original could flush: its ring words may pass a limit, or a take passes the pool threshold
    AllocCapacity, // an allocation may fail
    AllocWait,     // the null pool allocator would wait
    Diagnostics,   // a capture, harvest, scene session, route trace, effect sample or probe needs the Original body
};
inline constexpr std::size_t                                         kLowererFallbackCount = static_cast<std::size_t>(LowererFallback::Diagnostics) + 1;
inline constexpr std::array<std::string_view, kLowererFallbackCount> kLowererFallbackNames{
    "dispatcher",
    "nested",
    "stubs",
    "stack_overlap",
    "tiling",
    "unreadable",
    "state_dirty",
    "tiled_state",
    "alloc_marker",
    "tile_growth",
    "alloc_path",
    "ring_space",
    "alloc_capacity",
    "alloc_wait",
    "diagnostics",
};

// The cvar native_consume_lowerer.
bool LowererConsumeRequested() noexcept;
// The cvar native_consume_lowerer_tiled, effective only with the one above.
bool LowererTiledConsumeRequested() noexcept;

// The entry registers of the call.
LowererEntry LowererEntryOf(const PPCContext& ctx) noexcept;

// Everything a consumed call stores.
struct LowererPlan
{
    LowererEntry              entry;
    LowererWatch              watch;
    LowererExitState          exit;
    std::vector<LowererWrite> writes;
    bool                      tiled = false;
};

// An empty refusal means skip Original, with plan filled and root
// open. Dirty 0x1E0000 or stride change is StateDirty here;
// diagnostics are last, after state/read/model guards.
std::optional<LowererFallback> LowererConsumeVerdict(const NativeDrawScope& scope, const LowererEntry& entry, std::uint32_t caller, bool diagnostics, LowererPlan* plan = nullptr) noexcept;

// Lowerer consume skips Original only when the plan writes r3=0;
// diagnostics can retain the body.
bool ConsumeLowerer(const NativeDrawScope& scope, PPCContext& ctx, bool diagnostics) noexcept;

// A hook shared with other entries has no draw scope for them.
inline bool ConsumeLowerer(const NativeDrawUnobserved&, PPCContext&, bool) noexcept
{
    return false;
}

#if REREVVED_DEVELOPER
struct LowererConsumeCounts
{
    std::uint64_t                                    consumed = 0, consumedTiled = 0; // the tiled ones are also in consumed
    std::array<std::uint64_t, kLowererFallbackCount> fallback{};
};

// Returns the counts since the previous take and resets them.
LowererConsumeCounts TakeLowererConsumeCounts() noexcept;
// " draw_consumed_lowerer=N draw_consumed_lowerer_tiled=N", then
// " draw_fallback_lowerer_<reason>=N" for nonzero counts.
std::string FormatLowererConsumeCounts(const LowererConsumeCounts& counts);
#endif

// True when the bracket tile list is worth observing for the lowerer consume.
bool LowererTileListObservationRequested() noexcept;

#if REREVVED_DEVELOPER
// lowerer_derive.h supplies the entry/exit model for Original calls;
// the probe does not change execution.
class LowererExitProbe
{
public:
    explicit LowererExitProbe(const PPCContext& ctx) noexcept;
    ~LowererExitProbe();

    LowererExitProbe(const LowererExitProbe&)            = delete;
    LowererExitProbe& operator=(const LowererExitProbe&) = delete;

    // True while the probe holds an entry state to compare.
    bool Armed() const noexcept
    {
        return static_cast<bool>(state);
    }

private:
    struct State;
    std::unique_ptr<State> state;
    const PPCContext*      context    = nullptr;
    int                    exceptions = 0;
};

#endif
struct LowererExitUnobserved
{
};

// A probe only for the lowerer entry, so shared hook macros cost nothing for every other entry.
// The shipping build has no probe.
template <std::uint32_t Address>
auto ObserveLowererExit([[maybe_unused]] const PPCContext& ctx) noexcept
{
#if REREVVED_DEVELOPER
    if constexpr (LowererConsumeEntry(Address))
        return LowererExitProbe(ctx);
    else
#endif
        return LowererExitUnobserved{};
}

} // namespace rerevved::gpu::guest_d3d
