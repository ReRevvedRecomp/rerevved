#include "developer_hooks.h"
#include "resource_mirror_hooks.h"

#include "clear_consume.h"
#include "ring_stubs.h"
#include "set_predication_consume.h"

#include "gpu/completion/guest_progress_hooks.h"

#include <rex/hook.h>

// Counts (developer builds) and feeds mirrors before Original; only Clear 0x826A2970
// and SetPredication 0x8269CD20 may consume after progress setup. SetPredication
// exit probe follows consume, so consumed calls are never probed.
#define GUEST_API_HOOK_Census(address)                                                                                  \
    REX_EXTERN(__imp__sub_##address);                                                                                   \
    REX_HOOK_RAW(sub_##address)                                                                                         \
    {                                                                                                                   \
        [[maybe_unused]] const auto setPredicationCall =                                                                \
            rerevved::gpu::guest_d3d::ObserveSetPredicationCall<0x##address>(ctx);                                      \
        [[maybe_unused]] const auto guestApiCall   = rerevved::gpu::guest_d3d::CountGuestApiCall<0x##address>();        \
        [[maybe_unused]] const auto resourceMirror = rerevved::gpu::guest_d3d::ObserveResourceMirror<0x##address>(ctx); \
        rerevved::gpu::completion::ObserveGuestProgress<0x##address>(ctx);                                              \
        if constexpr (rerevved::gpu::guest_d3d::ClearConsumeEntry(0x##address))                                         \
        {                                                                                                               \
            if (rerevved::gpu::guest_d3d::ClearConsumeRequested() &&                                                    \
                rerevved::gpu::completion::detail::ConsumeGuestClear(                                                   \
                    ctx,                                                                                                \
                    rerevved::gpu::guest_d3d::DeveloperRecorderActive())) [[unlikely]]                                  \
                return;                                                                                                 \
        }                                                                                                               \
        if constexpr (rerevved::gpu::guest_d3d::SetPredicationConsumeEntry(0x##address))                                \
        {                                                                                                               \
            if (rerevved::gpu::guest_d3d::SetPredicationConsumeRequested() &&                                           \
                rerevved::gpu::guest_d3d::ConsumeSetPredication(                                                        \
                    ctx,                                                                                                \
                    rerevved::gpu::guest_d3d::RingStubsReady(ctx.r3.u32),                                               \
                    rerevved::gpu::guest_d3d::DeveloperRecorderActive())) [[unlikely]]                                  \
                return;                                                                                                 \
        }                                                                                                               \
        [[maybe_unused]] const auto setPredicationExit =                                                                \
            rerevved::gpu::guest_d3d::ObserveSetPredicationExit<0x##address>(ctx);                                      \
        setPredicationCall.Original();                                                                                  \
        __imp__sub_##address(ctx, base);                                                                                \
    }
#define GUEST_API_HOOK_Route(address)
#define GUEST_API_HOOK_Raw(address)
#define GUEST_API_ENTRY(address, role, name, hook) GUEST_API_HOOK_##hook(address)
#include "guest_api_entries.inc"
#undef GUEST_API_ENTRY
#undef GUEST_API_HOOK_Raw
#undef GUEST_API_HOOK_Route
#undef GUEST_API_HOOK_Census
