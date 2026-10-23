#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

#ifndef REREVVED_DEVELOPER
#error "REREVVED_DEVELOPER must be defined to 0 or 1 by the build"
#endif

#if REREVVED_DEVELOPER
#include "completion_probe.h"
#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#include "gpu/diagnostics/shader_harvest_hook.h"
#include "guest_api_census.h"
#include "tiling_probe.h"
#endif

namespace rerevved::gpu::guest_d3d
{

#if REREVVED_DEVELOPER

// A shader harvest, scene capture or route trace needs the guest's own body,
// so a hook that could skip Original asks this first.
[[nodiscard]] inline bool DeveloperRecorderActive() noexcept
{
    return diagnostics::shaderHarvestObserver.load(std::memory_order_acquire) != nullptr ||
           diagnostics::sceneCaptureSession.load(std::memory_order_relaxed) != nullptr ||
           diagnostics::routeTraceEnabled.load(std::memory_order_relaxed) != 0;
}

template <std::uint32_t Address>
[[nodiscard]] auto ObserveShaderHarvestEntry(const PPCContext& ctx) noexcept
{
    return diagnostics::ObserveShaderHarvest<Address>(ctx);
}

#else

// The shipping build has no recorders: these keep each hook's source shape and
// compile to nothing.
[[nodiscard]] constexpr bool DeveloperRecorderActive() noexcept
{
    return false;
}

struct GuestApiUncounted
{
};

template <std::uint32_t>
[[nodiscard]] constexpr GuestApiUncounted CountGuestApiCall() noexcept
{
    return {};
}

struct ShaderHarvestUnobserved
{
};

// The completion and tiling probes are developer-only. The ring stubs and route
// wrappers keep their flow in both builds, so the shipping build supplies the
// observation points they call as empty stand-ins.
enum class CompletionAdd : std::uint8_t
{
    Deferred,
    Direct,
    Stub
};

[[nodiscard]] constexpr std::uint32_t CompletionThread() noexcept
{
    return 0;
}

class CompletionScope
{
public:
    CompletionScope(std::uint32_t, const PPCContext&) noexcept
    {
    }

    void Stubbed() const noexcept
    {
    }
};

template <std::uint32_t>
[[nodiscard]] constexpr CompletionScope ObserveCompletionRoute(const PPCContext& context) noexcept
{
    return CompletionScope(0, context);
}

constexpr void ObserveCompletionStoreBegin(std::uint32_t) noexcept
{
}

constexpr void ObserveCompletionStoreEnd(std::uint32_t, std::uint32_t, std::uint32_t, CompletionAdd, std::uint32_t, std::uint32_t, bool = false) noexcept
{
}

constexpr void ObserveCompletionStubBegin() noexcept
{
}

constexpr void ObserveCompletionStubEnd(std::uint32_t, std::uint32_t, std::uint32_t, bool) noexcept
{
}

enum class TilingArm : std::uint8_t
{
    None,
    Deferred,
    Direct,
};

constexpr void NoteTilingSubmitArm(TilingArm) noexcept
{
}

class TilingSubmitScope
{
public:
    TilingSubmitScope(std::uint32_t, std::uint64_t, std::uint32_t) noexcept
    {
    }
};

class TilingBracketScope
{
public:
    TilingBracketScope(std::uint32_t, const std::uint64_t&) noexcept
    {
    }
};

template <std::uint32_t>
[[nodiscard]] constexpr ShaderHarvestUnobserved ObserveShaderHarvestEntry(const PPCContext&) noexcept
{
    return {};
}

#endif

} // namespace rerevved::gpu::guest_d3d
