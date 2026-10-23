#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gpu::diagnostics
{

enum class RouteFunction : std::size_t
{
#define ROUTE_ENTRY(address, groups, mode) sub_##address,
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
    Count
};

struct RouteEntry
{
    std::uint32_t    address;
    std::string_view groups;
    bool             existingHook;
};

inline constexpr RouteEntry kRouteEntries[]{
#define ROUTE_ENTRY(address, groups, mode) { 0x##address, groups, #mode == std::string_view("Existing") },
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
};

inline constexpr RouteFunction kRouteCountOnlyFunctions[]{
#define ROUTE_COUNT_ONLY(address) RouteFunction::sub_##address,
#include "route_trace_count_only.inc"
#undef ROUTE_COUNT_ONLY
};

struct RouteStorageIndex
{
    bool        countOnly;
    std::size_t index;
};

inline constexpr auto kRouteStorageIndices = []
{
    std::array<RouteStorageIndex, std::size(kRouteEntries)> result{};
    std::size_t                                             callerIndex = 0;
    for (std::size_t function = 0; function < result.size(); ++function)
    {
        bool countOnly = false;
        for (std::size_t counter = 0; counter < std::size(kRouteCountOnlyFunctions); ++counter)
        {
            if (function == static_cast<std::size_t>(kRouteCountOnlyFunctions[counter]))
            {
                result[function] = { true, counter };
                countOnly        = true;
                break;
            }
        }
        if (!countOnly)
            result[function] = { false, callerIndex++ };
    }
    return result;
}();

struct RouteDrawCaller
{
    std::uint32_t function, caller, owner;
};

inline constexpr RouteDrawCaller kRouteDrawCallers[]{
#define ROUTE_CALL(function, caller, owner) { function, caller, owner },
#include "route_trace_calls.inc"
#undef ROUTE_CALL
};

inline constexpr std::size_t kRouteCallerCapacity  = 64;
inline constexpr std::size_t kRouteSegmentCapacity = 4;
inline constexpr auto        kRouteNoFrame         = std::numeric_limits<std::uint64_t>::max();

struct RouteCallerCount
{
    std::size_t   function;
    std::uint64_t caller, count, segmentCount, firstFrame, segmentFirstFrame;
};

struct RouteOverflowCount
{
    std::size_t   function;
    std::uint64_t count, segmentCount;
};

struct RouteSnapshot
{
    std::uint64_t                   beginFrame = 0, frameCount = 0;
    std::vector<RouteCallerCount>   callers;
    std::vector<RouteOverflowCount> overflow;
    std::vector<RouteOverflowCount> countOnly;
};

// Only the recorder thread closes segments. Guest writers never wait for it.
// Closed banks remain immutable after writer registration has drained.
class RouteRecorder
{
public:
    void          Record(RouteFunction function, std::uint64_t caller) noexcept;
    void          FrameBoundary() noexcept;
    RouteSnapshot CloseSegment();

private:
    struct Bank
    {
        std::atomic<std::uint64_t> count{ 0 }, firstFrame{ kRouteNoFrame };
    };

    struct Slot
    {
        std::atomic<std::uint64_t>              caller{ kRouteNoFrame };
        std::array<Bank, kRouteSegmentCapacity> banks;
    };

    struct Function
    {
        std::array<Slot, kRouteCallerCapacity>                        slots;
        std::array<std::atomic<std::uint64_t>, kRouteSegmentCapacity> overflow{};
        std::array<std::atomic<std::uint64_t>, kRouteSegmentCapacity> writers{};
    };

    std::array<Function, std::size(kRouteEntries) - std::size(kRouteCountOnlyFunctions)> functions;
    std::array<std::atomic<std::uint64_t>, std::size(kRouteCountOnlyFunctions)>          countOnly{};
    std::array<std::uint64_t, std::size(kRouteCountOnlyFunctions)>                       previousCountOnly{};
    std::atomic<unsigned>                                                                segment{ 0 };
    std::atomic<std::uint64_t>                                                           frames{ 0 };
    std::uint64_t                                                                        beginFrame = 0;
};

bool WriteRouteSnapshot(const std::filesystem::path& directory,
                        std::string_view             segment,
                        const RouteSnapshot&         snapshot,
                        std::string_view             titleRevision,
                        std::string_view             sdkRevision,
                        std::string&                 error);
bool StartRouteTrace(const std::filesystem::path& directory,
                     std::string_view             titleRevision,
                     std::string_view             sdkRevision,
                     std::string&                 error);
void StopRouteTrace() noexcept;
void NotifyRouteTraceFrameBoundary() noexcept;

// Share the existing relaxed gate: 0 is disabled, 1 counts, 2 counts + effects.
inline constexpr unsigned          kRouteTraceEffects = 2;
extern std::atomic<unsigned>       routeTraceEnabled;
extern std::atomic<RouteRecorder*> routeTraceRecorder;

// Keep the disabled hook path to one relaxed load and branch.
inline bool TraceRouteEntry(RouteFunction function, std::uint64_t caller) noexcept
{
    const auto mode = routeTraceEnabled.load(std::memory_order_relaxed);
    if (mode)
    {
        // Pair the successful relaxed gate with its release store before loading the pointer.
        std::atomic_thread_fence(std::memory_order_acquire);
        routeTraceRecorder.load(std::memory_order_acquire)->Record(function, caller);
        return mode == kRouteTraceEffects;
    }
    return false;
}

} // namespace rerevved::gpu::diagnostics
