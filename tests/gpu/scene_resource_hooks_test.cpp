#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "guest_import_doubles.hpp"
#include "production_observers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <rex/hook.h>
#include <stdexcept>

using namespace rerevved::gpu::diagnostics;

namespace
{

unsigned   calls = 0, begins = 0, ends = 0;
PPCContext entryContext{};
bool       windowActive   = true;
bool       unwindOriginal = false;

void original(PPCContext& ctx, std::uint8_t* base)
{
    ++calls;
    ++*base;
    ++ctx.r3.u64;
    ctx.lr ^= 0x100;
    if (unwindOriginal)
        throw std::runtime_error("fixture Original unwind");
}

void check(void (*hook)(PPCContext&, std::uint8_t*))
{
    PPCContext ctx{};
    ctx.r3.u64    = 0x123456789ABCDEF0;
    ctx.lr        = 0x8250C7B4;
    entryContext  = ctx;
    auto expected = ctx;
    ++expected.r3.u64;
    expected.lr ^= 0x100;
    std::uint8_t memory = 0;
    const auto   before = calls;
    hook(ctx, &memory);
    UNSCOPED_INFO("chained wrapper calls Original exactly once");
    REQUIRE((calls == before + 1 && memory == 1));
    UNSCOPED_INFO("chained wrapper preserves all guest context effects");
    REQUIRE(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
}

} // namespace

// The route wrappers under test; the fixture Original is installed per case.
#define ORIGINAL_New(address) REX_EXTERN(sub_##address);
#define ORIGINAL_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ORIGINAL_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef ORIGINAL_New
#undef ORIGINAL_Existing

namespace
{

// What the observer saw. The hooks are noexcept, so the case asserts on these
// after each call.
unsigned returnLinkMismatches = 0, entryContextMismatches = 0;

} // namespace

TEST_CASE("scene_resource_hooks", "[ctest][scene-resource-hooks]")
{
    // Substitutes only the scene storage observers; production wrappers are linked.
    rerevved::tests::production_observers::SceneEntryObserver observer;
    observer.begin = [](void* session, std::uint32_t entry, const PPCContext& ctx) -> SceneHookToken
    {
        if (SceneEntryWindowOnly(entry) && !windowActive)
            return {};
        ++begins;
        entryContextMismatches += std::memcmp(&ctx, &entryContext, sizeof(ctx)) != 0;
        return { session, entry, ctx.r3.u32, begins, ctx.lr, {} };
    };
    observer.end = [](SceneHookToken token, const PPCContext& ctx)
    {
        if (!token.session)
            return;
        ++ends;
        returnLinkMismatches += !(token.caller == entryContext.lr && ctx.lr == (token.caller ^ 0x100));
    };
    rerevved::tests::production_observers::ScopedSceneEntryObserver observing(&observer);
    rerevved::tests::guest_imports::ScopedFallbackImport            originals([](std::uint32_t, PPCContext& ctx, std::uint8_t* base)
                                                                              {
                                                                       original(ctx, base);
                                                                              });

    // Leaves the process-wide capture and trace state as other cases expect it.
    struct Restore
    {
        ~Restore()
        {
            routeTraceEnabled.store(false, std::memory_order_relaxed);
            routeTraceRecorder.store(nullptr, std::memory_order_release);
            sceneCaptureSession.store(nullptr, std::memory_order_relaxed);
        }
    } restore;

    calls = begins = ends = 0;
    returnLinkMismatches = entryContextMismatches = 0;
    windowActive                                  = true;
    unwindOriginal                                = false;
    for (const auto entry : { 0x826B5F30U, 0x82521558U, 0x82305648U, 0x82C69850U, 0x826B7E00U, 0x827F8550U })
    {
        UNSCOPED_INFO("all writers share boot-to-scene observation");
        REQUIRE((SceneEntryObserved(entry) && !SceneEntryWindowOnly(entry)));
    }
    auto recorder = std::make_unique<RouteRecorder>();
    routeTraceRecorder.store(recorder.get(), std::memory_order_release);
    routeTraceEnabled.store(true, std::memory_order_release);
    sceneCaptureSession.store(recorder.get(), std::memory_order_release);
#define CHECK_New(address) check(sub_##address);
#define CHECK_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) CHECK_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
    unsigned expectedBegins = 0;
    for (const auto& entry : kRouteEntries)
        if (!entry.existingHook && SceneEntryObserved(entry.address))
            ++expectedBegins;
    UNSCOPED_INFO("route and scene share each selected new wrapper");
    REQUIRE((begins == expectedBegins && ends == begins));
    const auto snapshot = recorder->CloseSegment();
    UNSCOPED_INFO("route recorder remains independently enabled");
    REQUIRE(!snapshot.callers.empty());
    windowActive             = false;
    const auto outsideWindow = begins;
#define WINDOW_New(address)                          \
    if constexpr (SceneEntryWindowOnly(0x##address)) \
        check(sub_##address);
#define WINDOW_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) WINDOW_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef WINDOW_New
#undef WINDOW_Existing
    UNSCOPED_INFO("outside-window gating preserves Original effects");
    REQUIRE((begins == outsideWindow && ends == begins));
    PPCContext ctx{};
    ctx.lr                    = 0x82521AD0;
    entryContext              = ctx;
    std::uint8_t memory       = 0;
    const auto   beforeUnwind = calls;
    const auto   beforeEnd    = ends;
    unwindOriginal            = true;
    try
    {
        sub_826B5F30(ctx, &memory);
        UNSCOPED_INFO("Original exception must propagate");
        REQUIRE(false);
    }
    catch (const std::runtime_error&)
    {
        UNSCOPED_INFO("unwinding Original executes once and retains guest effects");
        REQUIRE((calls == beforeUnwind + 1 && memory == 1 && ctx.r3.u64 == 1));
        UNSCOPED_INFO("scope guard ends the scene observer on unwind");
        REQUIRE((ends == beforeEnd + 1 && ends == begins));
    }
    unwindOriginal = false;
    sceneCaptureSession.store(nullptr, std::memory_order_relaxed);
    const auto disabled = begins;
#define ROUTE_ENTRY(address, groups, mode) CHECK_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef CHECK_New
#undef CHECK_Existing
    UNSCOPED_INFO("disabled scene gate does not invoke observer");
    REQUIRE((begins == disabled && ends == disabled));
    routeTraceEnabled.store(false, std::memory_order_relaxed);
    UNSCOPED_INFO("scene observer runs before Original");
    REQUIRE(entryContextMismatches == 0);
    UNSCOPED_INFO("return observer retains entry LR separately");
    REQUIRE(returnLinkMismatches == 0);
}
