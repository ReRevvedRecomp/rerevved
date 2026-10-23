#pragma once

#include "indexed_buffer_consume.h"
#include "native_draw_hook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// User-pointer wrapper/consume map; native_consume_user_pointer requires
//   ConsumeStateVerdict.
// Wrapper                       | IndexedUp 0x826A3568 / NonindexedUp
//   0x826A3000.
// Host observation              | Reads caller vertex/index pointers and stride
//   at entry in dispatcher began, before Original.
// Original nested call          | Copies bytes into ring through lowerer/sink
//   (g188:13083, g26:13041), then publishes cursor; host never reads that copy.
// Consumed nested call          | Skipped; NoteNativeDrawConsumedChild records
//   it as made. Callers ignore returned copy destination.
// Tiled / dirty gates           | native_consume_user_pointer_tiled: byte
//   exactly 0x21; native_consume_user_pointer_dirty:
//   kIndexedBufferDirtyLowMask, | under kIndexedBufferBracketOffset and
//   kIndexedBufferDirtyLowOffset conditions.
// Preserved guest state         | Stride transient G+0x30F0, allocation outputs
//   and G+0x30 remain at entry; next Original reallocates.
// Replayed guest stores         | All five dirty-qword clears; then G+0x18 = 1
//   only for proved successful reservation.
// Unknown/failure reservation   | Runs Original. Numbering, ledger record and
//   native submission remain unchanged.

enum class UserPointerWrapper : std::uint8_t
{
    IndexedUp,    // 0x826A3568
    NonindexedUp, // 0x826A3000
};
// Wrapper checks device through G+0x3A58, VS pointer G+0x3198 (header+872,
// VS+40/VS+456), declaration G+0x2E2C (+32..+51). Lowerer vertex-out word r1+84
// lies inside wrapper (g63:13775,13798).
inline constexpr std::size_t kUserPointerWrapperCount = static_cast<std::size_t>(UserPointerWrapper::NonindexedUp) + 1;

#if REREVVED_DEVELOPER
inline constexpr std::array<std::string_view, kUserPointerWrapperCount> kUserPointerWrapperNames{
    "indexed_up",
    "nonindexed_up",
};
#endif

// The reasons are those of the IndexedBuffer verdict, in the same order.
using UserPointerFallback = IndexedBufferFallback;

// stack_guard.h checks reach[limit,r1+88): IndexedUp stride r1+84 (260 under
// 176-byte frame,g188:13053); NonindexedUp no above-r1 use. Frames 176/128
// bytes (g188:13049,g26:13025); insufficient stack refuses.
inline constexpr std::array<StackFootprint, 2> kUserPointerStackFootprints{ { { 88, 0x3A5C, 176 }, { 88, 0x3A5C, 128 } } };

// The cvar native_consume_user_pointer while ring stub services are published;
// a consume does nothing without them.
bool UserPointerConsumeRequested() noexcept;
// The cvar native_consume_user_pointer_tiled, effective only with the one above.
bool UserPointerTiledConsumeRequested() noexcept;
// The cvar native_consume_user_pointer_dirty, effective only with the base cvar.
bool UserPointerDirtyConsumeRequested() noexcept;

// Wrapper entry stride: IndexedUp r1+84(g188:13050, loaded 260(r1) after
// 176-byte frame), NonindexedUp r7(g26:13030). Nested call compares it with
// cached byte G+0x2E38.
inline constexpr std::uint32_t kIndexedUpStrideSlot = 84;
EntryStride                    UserPointerEntryStride(UserPointerWrapper wrapper, const PPCContext& ctx) noexcept;

// An empty result means the IndexedUp/NonindexedUp root may skip Original;
// diagnostics retains body captures.
std::optional<UserPointerFallback> UserPointerConsumeVerdict(UserPointerWrapper wrapper, const NativeDrawScope& scope, std::uint32_t graphics, const EntryStride& stride, bool diagnostics, bool* tiled = nullptr, bool* dirty = nullptr, bool* vertexFetch = nullptr) noexcept;

// User-pointer consume requires cvar and published services, validates dirty
// destinations and reservation, and records skipped children.
bool ConsumeUserPointer(UserPointerWrapper wrapper, const NativeDrawScope& scope, const PPCContext& ctx, bool diagnostics) noexcept;

#if REREVVED_DEVELOPER
struct UserPointerConsumeCounts
{
    std::array<std::uint64_t, kUserPointerWrapperCount>                                          consumed{};                   // per wrapper; includes the tiled and dirty ones
    std::uint64_t                                                                                consumedTiled            = 0; // both wrappers; includes tiled dirty ones
    std::uint64_t                                                                                consumedDirty            = 0; // both wrappers; includes tiled dirty ones
    std::uint64_t                                                                                consumedDirtyVertexFetch = 0; // both wrappers; dirty roots the dirty vertex fetch cvar admitted
    std::array<std::array<std::uint64_t, kIndexedBufferFallbackCount>, kUserPointerWrapperCount> fallback{};
};

// Returns the counts since the previous take and resets them.
UserPointerConsumeCounts TakeUserPointerConsumeCounts() noexcept;

std::string FormatUserPointerConsumeCounts(const UserPointerConsumeCounts& counts);
#endif

} // namespace rerevved::gpu::guest_d3d
