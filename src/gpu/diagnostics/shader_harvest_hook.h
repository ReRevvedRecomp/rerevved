#pragma once

#include "shader_harvest.h"

#include "gpu/guest_d3d/native_draw_hook.h"

#include <rex/hook.h>

namespace rerevved::gpu::diagnostics
{

// Route-wrapped entries the shader harvest observes: the draw entries the
// draw capture does not wrap (IndexedUpLowerer, NonindexedSink and
// AutomaticIndex), BeginTiling and D3DDevice_Resolve.
constexpr bool ShaderHarvestRouteEntry(std::uint32_t address) noexcept
{
    using guest_d3d::NativeDrawHook;
    using guest_d3d::NativeDrawHookAddress;
    return address == NativeDrawHookAddress(NativeDrawHook::IndexedUpLowerer) || address == NativeDrawHookAddress(NativeDrawHook::NonindexedSink) ||
           address == NativeDrawHookAddress(NativeDrawHook::AutomaticIndex) || address == kHarvestBeginTilingEntry || address == kHarvestResolveEntry;
}

// Reports the entry after Original returns, with r3 as its result; a
// resolve's arguments are reported at entry, before its copy words.
class ShaderHarvestScope
{
public:
    ShaderHarvestScope(std::uint32_t entry, const PPCContext& ctx) noexcept
    {
        if (const auto harvest = shaderHarvestObserver.load(std::memory_order_acquire)) [[unlikely]]
        {
            call = { entry, static_cast<std::uint32_t>(ctx.lr), { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 }, ctx.r1.u32 };
            if (entry == kHarvestResolveEntry)
                harvest(call);
            else
            {
                observer = harvest;
                context  = &ctx;
            }
        }
    }

    ~ShaderHarvestScope()
    {
        if (observer)
        {
            call.result = context->r3.u32;
            observer(call);
        }
    }

    ShaderHarvestScope(const ShaderHarvestScope&)            = delete;
    ShaderHarvestScope& operator=(const ShaderHarvestScope&) = delete;

private:
    ShaderHarvestObserver observer = nullptr;
    ShaderHarvestCall     call;
    const PPCContext*     context = nullptr;
};

struct ShaderHarvestUnobserved
{
};

template <std::uint32_t Address>
auto ObserveShaderHarvest(const PPCContext& ctx) noexcept
{
    if constexpr (ShaderHarvestRouteEntry(Address))
        return ShaderHarvestScope(Address, ctx);
    else
        return ShaderHarvestUnobserved{};
}

} // namespace rerevved::gpu::diagnostics
