#include "guest_import_doubles.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <rex/hook.h>

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/route_trace.h"
#endif

REX_EXTERN(sub_826A3568);
REX_EXTERN(sub_826AD150);
REX_EXTERN(sub_826A3000);
REX_EXTERN(sub_826A39F8);
REX_EXTERN(sub_826A7040);
REX_EXTERN(sub_826A7138);
REX_EXTERN(sub_826A2FF0);

void ObserveNativeGuestGammaTable(PPCRegister& r31, PPCRegister& r1);
void ObserveNativeGuestPwlGamma(PPCRegister& r31);

namespace
{

unsigned originalCalls = 0;

void checkOriginal(void (*hook)(PPCContext&, std::uint8_t*), std::uint64_t caller = 0x1234)
{
    PPCContext ctx{};
    ctx.r1.u64    = 0x12345678;
    ctx.r3.u64    = 0x1111222233334444ULL;
    ctx.r4.u64    = 4;
    ctx.lr        = caller;
    auto expected = ctx;
    expected.r3.u64 += 1;
    expected.lr ^= 0x100;
    std::uint8_t memory = 0;
    const auto   calls  = originalCalls;
    hook(ctx, &memory);
    UNSCOPED_INFO("original draw must run exactly once");
    REQUIRE((originalCalls == calls + 1 && memory == 1));
    UNSCOPED_INFO("the wrapper must preserve original context effects");
    REQUIRE(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
}

} // namespace

TEST_CASE("draw_hooks", "[ctest][draw-hooks]")
{
#if REREVVED_DEVELOPER
    using namespace rerevved::gpu::diagnostics;
#endif
    // The Original of every raw draw entry: count the call and flip the link.
    rerevved::tests::guest_imports::ScopedFallbackImport original([](std::uint32_t, PPCContext& ctx, std::uint8_t* base)
                                                                  {
                                                                      ++originalCalls;
                                                                      ctx.r3.u64 += 1;
                                                                      ctx.lr ^= 0x100;
                                                                      base[0] += 1;
                                                                  });

#if REREVVED_DEVELOPER
    // Leaves the process-wide trace state as other cases expect it.
    struct Restore
    {
        ~Restore()
        {
            routeTraceEnabled.store(false, std::memory_order_relaxed);
            routeTraceRecorder.store(nullptr, std::memory_order_release);
        }
    } restore;

    originalCalls      = 0;
    auto routeRecorder = std::make_unique<RouteRecorder>();
    routeTraceRecorder = routeRecorder.get();
    routeTraceEnabled.store(true, std::memory_order_relaxed);
#else
    originalCalls = 0;
#endif

    PPCRegister gammaDevice{}, gammaStack{};
    gammaDevice.u32 = 1;
    gammaStack.u32  = 0xFFFFFFF0;
    UNSCOPED_INFO("a disabled gamma observer must not touch guest memory");
    ObserveNativeGuestGammaTable(gammaDevice, gammaStack);
    ObserveNativeGuestPwlGamma(gammaDevice);

    checkOriginal(sub_826A3568);
    checkOriginal(sub_826AD150);
    checkOriginal(sub_826A3000, 0x82304258);
    checkOriginal(sub_826A39F8);
    checkOriginal(sub_826A7040);
    checkOriginal(sub_826A7138);
    checkOriginal(sub_826A2FF0);

#if REREVVED_DEVELOPER
    routeTraceEnabled.store(false, std::memory_order_relaxed);
    const auto          route = routeRecorder->CloseSegment();
    std::array<bool, 6> chained{};
    const std::array    functions{ RouteFunction::sub_826A3568, RouteFunction::sub_826A3000, RouteFunction::sub_826A39F8, RouteFunction::sub_826AD150, RouteFunction::sub_826A7040, RouteFunction::sub_826A7138 };
    for (const auto& row : route.callers)
        for (std::size_t i = 0; i < functions.size(); ++i)
            chained[i] = chained[i] || row.function == static_cast<std::size_t>(functions[i]);
    UNSCOPED_INFO("all six traced draw hooks must chain to the enabled route trace without changing Original effects");
    REQUIRE(std::all_of(chained.begin(), chained.end(), [](bool seen)
                        {
                            return seen;
                        }));
#endif
}
