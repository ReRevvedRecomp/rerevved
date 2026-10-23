#include "developer_hooks.h"

#include "lowerer_consume.h"
#include "native_draw_hook.h"
#include "resolve_consume.h"
#include "resource_mirror_hooks.h"
#include "ring_stubs.h"

#include "gpu/completion/guest_progress_hooks.h"

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/route_effect_trace.h"
#endif

#include <rex/hook.h>

#if REREVVED_DEVELOPER

// The scene observation body is expanded at each call site so neither the disabled nor the
// enabled path depends on the compiler inlining a helper.
#define ROUTE_HOOK_BODY(address)                                                                           \
    if constexpr (rerevved::gpu::diagnostics::SceneEntryObserved(0x##address))                             \
    {                                                                                                      \
        if (auto* scene = rerevved::gpu::diagnostics::sceneCaptureSession.load(std::memory_order_relaxed)) \
        {                                                                                                  \
            std::atomic_thread_fence(std::memory_order_acquire);                                           \
            struct SceneEndGuard                                                                           \
            {                                                                                              \
                rerevved::gpu::diagnostics::SceneHookToken token;                                          \
                const PPCContext&                          context;                                        \
                ~SceneEndGuard()                                                                           \
                {                                                                                          \
                    if (token.session)                                                                     \
                        rerevved::gpu::diagnostics::EndSceneEntry(token, context);                         \
                }                                                                                          \
            } guard{ rerevved::gpu::diagnostics::BeginSceneEntry(scene, 0x##address, ctx), ctx };          \
            __imp__sub_##address(ctx, base);                                                               \
            return;                                                                                        \
        }                                                                                                  \
    }                                                                                                      \
    __imp__sub_##address(ctx, base)

// Original under the route trace, the route effect reader and the scene capture.
#define ROUTE_HOOK_RUN(address)                                                             \
    if (rerevved::gpu::diagnostics::TraceRouteEntry(                                        \
            rerevved::gpu::diagnostics::RouteFunction::sub_##address, ctx.lr)) [[unlikely]] \
    {                                                                                       \
        struct EffectEndGuard                                                               \
        {                                                                                   \
            rerevved::gpu::diagnostics::RouteEffectReaderToken token;                       \
            ~EffectEndGuard()                                                               \
            {                                                                               \
                if (token.recorder)                                                         \
                    rerevved::gpu::diagnostics::EndRouteEffectReader(token);                \
            }                                                                               \
        } effect{ rerevved::gpu::diagnostics::BeginRouteEffectReader(0x##address, ctx) };   \
        ROUTE_HOOK_BODY(address);                                                           \
        return;                                                                             \
    }                                                                                       \
    ROUTE_HOOK_BODY(address)

#else

#define ROUTE_HOOK_RUN(address) __imp__sub_##address(ctx, base)

#endif

// Progress observers run before ring stubs so the ledger sees fences and callbacks; replacing
// Original skips later observers.
#define ROUTE_HOOK_New(address)                                                                                           \
    REX_EXTERN(__imp__sub_##address);                                                                                     \
    REX_HOOK_RAW(sub_##address)                                                                                           \
    {                                                                                                                     \
        [[maybe_unused]] const auto ringWait =                                                                            \
            rerevved::gpu::guest_d3d::ObserveRingWait<0x##address>(ctx.r3.u32, ctx.lr, ctx.r4.u32);                       \
        [[maybe_unused]] const auto completionProbe = rerevved::gpu::guest_d3d::ObserveCompletionRoute<0x##address>(ctx); \
        [[maybe_unused]] const auto guestApiCall =                                                                        \
            rerevved::gpu::guest_d3d::CountGuestApiCall<0x##address>();                                                   \
        [[maybe_unused]] const auto resourceMirror =                                                                      \
            rerevved::gpu::guest_d3d::ObserveResourceMirror<0x##address>(ctx);                                            \
        rerevved::gpu::completion::ObserveGuestProgress<0x##address>(ctx);                                                \
        if constexpr (rerevved::gpu::guest_d3d::RingStubEntry(0x##address))                                               \
        {                                                                                                                 \
            if (rerevved::gpu::guest_d3d::ringStubsActive.load(std::memory_order_relaxed) &&                              \
                rerevved::gpu::guest_d3d::RunRingStub(0x##address, ctx, base)) [[unlikely]]                               \
            {                                                                                                             \
                completionProbe.Stubbed();                                                                                \
                return;                                                                                                   \
            }                                                                                                             \
        }                                                                                                                 \
        if constexpr (rerevved::gpu::guest_d3d::ResolveConsumeEntry(0x##address))                                         \
        {                                                                                                                 \
            if (rerevved::gpu::guest_d3d::ResolveConsumeRequested() &&                                                    \
                rerevved::gpu::completion::detail::ConsumeGuestResolve(                                                   \
                    ctx,                                                                                                  \
                    rerevved::gpu::guest_d3d::DeveloperRecorderActive())) [[unlikely]]                                    \
                return;                                                                                                   \
        }                                                                                                                 \
        [[maybe_unused]] const auto nativeDraw =                                                                          \
            rerevved::gpu::guest_d3d::ObserveNativeDraw<0x##address>(ctx, base);                                          \
        [[maybe_unused]] const auto shaderHarvest =                                                                       \
            rerevved::gpu::guest_d3d::ObserveShaderHarvestEntry<0x##address>(ctx);                                        \
        if constexpr (rerevved::gpu::guest_d3d::LowererConsumeEntry(0x##address))                                         \
        {                                                                                                                 \
            if (rerevved::gpu::guest_d3d::LowererConsumeRequested() &&                                                    \
                rerevved::gpu::guest_d3d::ConsumeLowerer(                                                                 \
                    nativeDraw,                                                                                           \
                    ctx,                                                                                                  \
                    rerevved::gpu::guest_d3d::DeveloperRecorderActive())) [[unlikely]]                                    \
                return;                                                                                                   \
        }                                                                                                                 \
        [[maybe_unused]] const auto lowererExit =                                                                         \
            rerevved::gpu::guest_d3d::ObserveLowererExit<0x##address>(ctx);                                               \
        ROUTE_HOOK_RUN(address);                                                                                          \
    }
#define ROUTE_HOOK_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ROUTE_HOOK_##mode(address)
#include "route_entries.inc"
#undef ROUTE_ENTRY
#undef ROUTE_HOOK_Existing
#undef ROUTE_HOOK_New
#undef ROUTE_HOOK_RUN
#if REREVVED_DEVELOPER
#undef ROUTE_HOOK_BODY
#endif
