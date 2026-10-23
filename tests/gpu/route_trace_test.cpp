#include "gpu/diagnostics/route_trace.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "guest_import_doubles.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <toml++/toml.hpp>

using namespace rerevved::gpu::diagnostics;

namespace
{

unsigned originalCalls = 0;

void original(PPCContext& ctx, std::uint8_t* base)
{
    ++originalCalls;
    ++*base;
    ++ctx.r3.u64;
    ctx.lr ^= 0x100;
}

void checkHook(void (*hook)(PPCContext&, std::uint8_t*))
{
    PPCContext context{};
    context.r3.u64 = 0x123456789ABCDEF0;
    context.lr     = 0x8250C7B4;
    auto expected  = context;
    ++expected.r3.u64;
    expected.lr ^= 0x100;
    std::uint8_t memory = 0;
    const auto   before = originalCalls;
    hook(context, &memory);
    UNSCOPED_INFO("each raw wrapper must call Original exactly once");
    REQUIRE((originalCalls == before + 1 && memory == 1));
    UNSCOPED_INFO("route wrapper must preserve every Original context effect");
    REQUIRE(std::memcmp(&context, &expected, sizeof(context)) == 0);
}

const RouteOverflowCount& countOnlyRow(const RouteSnapshot& snapshot, RouteFunction function)
{
    const auto row = std::find_if(snapshot.countOnly.begin(), snapshot.countOnly.end(), [function](const auto& value)
                                  {
                                      return value.function == static_cast<std::size_t>(function);
                                  });
    UNSCOPED_INFO("snapshot must include each count-only function");
    REQUIRE(row != snapshot.countOnly.end());
    return *row;
}

} // namespace

// The route wrappers under test.
#define ORIGINAL_New(address) REX_EXTERN(sub_##address);
#define ORIGINAL_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ORIGINAL_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef ORIGINAL_Existing
#undef ORIGINAL_New

namespace
{

// Installs the Original fixtures and restores the process-wide trace state.
class RouteTraceScope
{
public:
    RouteTraceScope() = default;

    ~RouteTraceScope()
    {
        routeTraceEnabled.store(false, std::memory_order_relaxed);
        routeTraceRecorder.store(nullptr, std::memory_order_release);
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
    }

    RouteTraceScope(const RouteTraceScope&)            = delete;
    RouteTraceScope& operator=(const RouteTraceScope&) = delete;

private:
    rerevved::tests::guest_imports::ScopedFallbackImport fallback_{ [](std::uint32_t, PPCContext& ctx, std::uint8_t* base)
                                                                    {
                                                                        original(ctx, base);
                                                                    } };
};

} // namespace

TEST_CASE("route_trace", "[ctest][route-trace]")
{
    using namespace rerevved::gpu::guest_d3d;
    RouteTraceScope scope;
    originalCalls = 0;
    std::set<std::uint32_t> addresses;
    unsigned                readers = 0, producers = 0, draws = 0, existing = 0;
    for (const auto& entry : kRouteEntries)
    {
        UNSCOPED_INFO("trace functions must have a single override/table row");
        REQUIRE(addresses.insert(entry.address).second);
        readers += entry.groups.find("result_reader") != std::string_view::npos;
        producers += entry.groups.find("fixed_state_producer") != std::string_view::npos;
        draws += entry.groups.find("draw_entry") != std::string_view::npos;
        existing += entry.existingHook;
    }
    UNSCOPED_INFO("census groups and shared hook ownership must be complete");
    REQUIRE((readers == 33 && producers == 102 && draws == 4 && existing == 6));
    UNSCOPED_INFO("all 17 known draw return sites are required");
    REQUIRE(std::size(kRouteDrawCallers) == 17);
    std::set<std::uint32_t> returns;
    for (const auto& call : kRouteDrawCallers)
    {
        UNSCOPED_INFO("draw caller returns must be unique, aligned and tied to a traced entry");
        REQUIRE((addresses.contains(call.function) && returns.insert(call.caller).second && call.caller % 4 == 0));
    }

    auto recorder = std::make_unique<RouteRecorder>();
    routeTraceRecorder.store(recorder.get(), std::memory_order_release);
#define CHECK_New(address) checkHook(sub_##address);
#define CHECK_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) CHECK_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
    const auto disabled = recorder->CloseSegment();
    UNSCOPED_INFO("disabled wrappers must not record or need a session");
    REQUIRE((disabled.callers.empty() && std::all_of(disabled.countOnly.begin(), disabled.countOnly.end(), [](const auto& row)
                                                     {
                                                         return row.count == 0;
                                                     })));
    routeTraceEnabled.store(true, std::memory_order_release);
#define ROUTE_ENTRY(address, groups, mode) CHECK_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef CHECK_Existing
#undef CHECK_New
    routeTraceEnabled.store(false, std::memory_order_relaxed);
    const auto hooks = recorder->CloseSegment();
    UNSCOPED_INFO("caller-enabled overrides must record entry before Original changes LR");
    REQUIRE(hooks.callers.size() == std::size(kRouteEntries) - 6 - std::size(kRouteCountOnlyFunctions));
    UNSCOPED_INFO("all five hot entries must retain count-only evidence");
    REQUIRE(hooks.countOnly.size() == 5);
    for (const auto& row : hooks.countOnly)
    {
        UNSCOPED_INFO("count-only hooks must record one call without a caller table");
        REQUIRE((row.count == 1 && row.segmentCount == 1 && kRouteStorageIndices[row.function].countOnly));
    }
    for (const auto& row : hooks.callers)
    {
        UNSCOPED_INFO("hook must record entry LR exactly");
        REQUIRE((row.caller == 0x8250C7B4 && row.count == 1 && row.segmentCount == 1));
    }

    recorder            = std::make_unique<RouteRecorder>();
    const auto function = RouteFunction::sub_826A3608;
    for (unsigned i = 0; i < kRouteCallerCapacity; ++i)
        recorder->Record(function, i * 4);
    recorder->Record(function, 0);
    recorder->Record(function, 0x9999);
    recorder->FrameBoundary();
    const auto boot = recorder->CloseSegment();
    UNSCOPED_INFO("capacity and VdSwap frame count must be bounded");
    REQUIRE((boot.callers.size() == kRouteCallerCapacity && boot.frameCount == 1));
    UNSCOPED_INFO("new caller at capacity must count overflow");
    REQUIRE(boot.overflow[static_cast<std::size_t>(function)].count == 1);
    recorder->Record(function, 0);
    recorder->Record(function, 0x9999);
    recorder->FrameBoundary();
    const auto menu = recorder->CloseSegment();
    const auto zero = std::find_if(menu.callers.begin(), menu.callers.end(), [](const auto& row)
                                   {
                                       return row.caller == 0;
                                   });
    UNSCOPED_INFO("segment snapshots must preserve cumulative and segment counts/first frames, including LR zero");
    REQUIRE((zero != menu.callers.end() && zero->count == 3 && zero->segmentCount == 1 && zero->firstFrame == 0 && zero->segmentFirstFrame == 1));
    UNSCOPED_INFO("overflow must be cumulative and segmented");
    REQUIRE((menu.beginFrame == 1 && menu.frameCount == 2 && menu.overflow[static_cast<std::size_t>(function)].count == 2 &&
             menu.overflow[static_cast<std::size_t>(function)].segmentCount == 1));
    recorder->Record(function, 0x9999);
    const auto overflowOnly = recorder->CloseSegment();
    UNSCOPED_INFO("execution through only overflow must remain visible in a segment");
    REQUIRE((std::all_of(overflowOnly.callers.begin(), overflowOnly.callers.end(), [](const auto& row)
                         {
                             return row.segmentCount == 0;
                         }) &&
             overflowOnly.overflow[static_cast<std::size_t>(function)].segmentCount == 1));
    bool exhausted = false;
    try
    {
        recorder->CloseSegment();
    }
    catch (const std::runtime_error&)
    {
        exhausted = true;
    }
    UNSCOPED_INFO("segment capacity must fail visibly");
    REQUIRE(exhausted);

    recorder = std::make_unique<RouteRecorder>();
    for (const auto hot : kRouteCountOnlyFunctions)
    {
        for (unsigned caller = 0; caller < kRouteCallerCapacity + 10; ++caller)
            recorder->Record(hot, caller * 4);
    }
    const auto hotBoot = recorder->CloseSegment();
    UNSCOPED_INFO("hot entries must never allocate caller rows");
    REQUIRE(hotBoot.callers.empty());
    for (const auto& row : hotBoot.countOnly)
    {
        UNSCOPED_INFO("hot entries must count every caller without capacity overflow");
        REQUIRE((row.count == kRouteCallerCapacity + 10 && row.segmentCount == row.count));
    }
    for (const auto hot : kRouteCountOnlyFunctions)
        recorder->Record(hot, kRouteNoFrame);
    const auto hotMenu = recorder->CloseSegment();
    for (const auto& row : hotMenu.countOnly)
    {
        UNSCOPED_INFO("count-only snapshots must conserve sampled cumulative and interval counts");
        REQUIRE((row.count == kRouteCallerCapacity + 11 && row.segmentCount == 1));
    }
    UNSCOPED_INFO("count-only calls must not masquerade as overflow");
    REQUIRE(std::all_of(hotMenu.overflow.begin(), hotMenu.overflow.end(), [](const auto& row)
                        {
                            return row.count == 0;
                        }));

    recorder = std::make_unique<RouteRecorder>();
    std::array<std::jthread, 4> writers;
    for (auto& writer : writers)
        writer = std::jthread([&]
                              {
                                  for (unsigned i = 0; i < 10000; ++i)
                                  {
                                      recorder->Record(function, 0x1234);
                                      recorder->Record(RouteFunction::sub_827F8550, 0x1234);
                                  }
                              });
    const auto concurrentBoot = recorder->CloseSegment();
    const auto concurrentMenu = recorder->CloseSegment();
    for (auto& writer : writers)
        writer.join();
    const auto final = recorder->CloseSegment();
    UNSCOPED_INFO("concurrent callers and segment closure must conserve counts without duplicate slots");
    REQUIRE((final.callers.size() == 1 && final.callers[0].count == 40000));
    const auto hotFunction = RouteFunction::sub_827F8550;
    UNSCOPED_INFO("concurrent count-only samples must conserve calls across intervals");
    REQUIRE((countOnlyRow(final, hotFunction).count == 40000 && countOnlyRow(concurrentBoot, hotFunction).segmentCount + countOnlyRow(concurrentMenu, hotFunction).segmentCount + countOnlyRow(final, hotFunction).segmentCount == 40000));

    const auto  directory = std::filesystem::temp_directory_path() / ("rerevved-route-test-" +
                                                                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("fresh route session must start");
    REQUIRE(StartRouteTrace(directory, "title-fixture clean", "sdk-fixture clean", error));
    TraceRouteEntry(function, 0x8250C7B4);
    TraceRouteEntry(RouteFunction::sub_827F8550, 0x8250C7B4);
    NotifyRouteTraceFrameBoundary();
    std::ofstream(directory / "menu").close();
    const auto waitSnapshot = [&](const char* name)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!std::filesystem::is_regular_file(directory / name) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("background marker must produce a snapshot without a guest pause");
        REQUIRE(std::filesystem::is_regular_file(directory / name));
    };
    waitSnapshot("route-menu.toml");
    TraceRouteEntry(function, 0x8250C7B4);
    std::ofstream(directory / "gameplay").close();
    waitSnapshot("route-gameplay.toml");
    StopRouteTrace();
    const auto saved = toml::parse_file((directory / "route-shutdown.toml").string());
    UNSCOPED_INFO("snapshot must preserve revisions and actual swap epochs");
    REQUIRE((saved["title_revision"].value_or(std::string{}) == "title-fixture clean" &&
             saved["sdk_revision"].value_or(std::string{}) == "sdk-fixture clean" &&
             saved["frame_count"].value_or(0) == 1));
    UNSCOPED_INFO("shutdown must retain cumulative history with an empty final segment");
    REQUIRE((saved["callers"][0]["count"].value_or(0) == 2 && saved["callers"][0]["segment_count"].value_or(-1) == 0));
    bool savedHotFunction = false;
    for (const auto& row : *saved["count_only"].as_array())
    {
        if (row.as_table()->at("function").value_or(0ULL) != 0x827F8550)
            continue;
        savedHotFunction = true;
        UNSCOPED_INFO("snapshots must serialize count-only execution and its sampled boundary");
        REQUIRE((row.as_table()->at("count").value_or(0) == 1 && row.as_table()->at("boundary").value_or(std::string{}) == "sampled relaxed counter"));
    }
    UNSCOPED_INFO("serialized snapshot must retain the hot copy function");
    REQUIRE(savedHotFunction);
    UNSCOPED_INFO("capture must not reuse a directory/session");
    REQUIRE(!StartRouteTrace(directory, "title", "sdk", error));
    UNSCOPED_INFO("snapshots must never overwrite evidence");
    REQUIRE(!WriteRouteSnapshot(directory, "shutdown", final, "title", "sdk", error));
    for (const auto* name : { "menu", "gameplay", "route-menu.toml", "route-gameplay.toml", "route-shutdown.toml", "route-shutdown.pending" })
        std::filesystem::remove(directory / name);
    std::filesystem::remove(directory);
}
