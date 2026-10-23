#pragma once

#include "gpu/guest_d3d/guest_resource_observers.h"
#include "gpu/guest_d3d/resolve_surface_hooks.h"
#include "gpu/guest_d3d/resource_mirror_registry.h"

#include <cstdint>
#include <optional>

#include <rex/ppc/context.h>

namespace rerevved::gpu::guest_d3d
{

// Resource hooks observe after Original, except surface locks read flags at
// entry; Original always runs.
enum class GuestResourceHook : std::uint8_t
{
    CreateTexture,      // 0x8269AB90, returns the 0x34-byte header
    CreateVertexBuffer, // 0x826982A8, returns the 0x20-byte header
    CreateIndexBuffer,  // 0x826983D0, returns the 0x20-byte header
    XgVertexBuffer,     // 0x826B6C40, header in r7
    XgIndexBuffer,      // 0x826B6CD8, header in r8
    PatchTextureBase,   // 0x826B7140, header in r3; the XGraphics texture builders' base
    PatchResourceBase,  // 0x826B71A8, header in r3; buffers here, textures through 0x826B7140
    SetTexture,         // 0x8269AF10, header in r5 (0 unbinds)
    SetStreamSource,    // 0x8268FB08, header in r5
    SetIndices,         // 0x8268FCB0, header in r4
    LockCore,           // 0x82697CA0, every lock entry; r3 the object, a level names its texture at +0x18
    UnlockCore,         // 0x82697F38, every unlock entry; r3 the texture or buffer
    ReleaseResource,    // 0x826981D8, returns the new reference count
    ReleaseDevice,      // 0x8268CE38, returns the new reference count
    FreePhysical,       // 0x8280DEC8, r3 the address MmFreePhysicalMemory frees
    EndTiling,          // 0x8269D320, ends a predicated-tiling bracket; surfaces only
};

constexpr std::optional<GuestResourceHook> FindGuestResourceHook(std::uint32_t address) noexcept
{
    switch (address)
    {
        case 0x8269AB90:
            return GuestResourceHook::CreateTexture;
        case 0x826982A8:
            return GuestResourceHook::CreateVertexBuffer;
        case 0x826983D0:
            return GuestResourceHook::CreateIndexBuffer;
        case 0x826B6C40:
            return GuestResourceHook::XgVertexBuffer;
        case 0x826B6CD8:
            return GuestResourceHook::XgIndexBuffer;
        case 0x826B7140:
            return GuestResourceHook::PatchTextureBase;
        case 0x826B71A8:
            return GuestResourceHook::PatchResourceBase;
        case 0x8269AF10:
            return GuestResourceHook::SetTexture;
        case 0x8268FB08:
            return GuestResourceHook::SetStreamSource;
        case 0x8268FCB0:
            return GuestResourceHook::SetIndices;
        case 0x82697CA0:
            return GuestResourceHook::LockCore;
        case 0x82697F38:
            return GuestResourceHook::UnlockCore;
        case 0x826981D8:
            return GuestResourceHook::ReleaseResource;
        case 0x8268CE38:
            return GuestResourceHook::ReleaseDevice;
        case 0x8280DEC8:
            return GuestResourceHook::FreePhysical;
        case 0x8269D320:
            return GuestResourceHook::EndTiling;
        default:
            return std::nullopt;
    }
}

// Feeds the published registry once Original has returned. `argument` is the
// object register captured at entry and `extent` the allocation size a free
// captured there; the result is read from ctx.r3.
void ObserveGuestResourceCall(GuestResourceHook hook, std::uint32_t argument, std::uint32_t extent, const PPCContext& ctx) noexcept;

// The size of the heap allocation at a guest address, as the SDK physical
// heap invalidates it on release; 0 when the lookup fails.
std::uint32_t GuestAllocationExtent(std::uint32_t address) noexcept;

// Held by a hook across Original, so its destructor observes the result.
class GuestResourceScope
{
public:
    GuestResourceScope(GuestResourceHook hook, const PPCContext& ctx) noexcept
    {
        // Only the surface registry observes EndTiling; the hook is a
        // constant at each call, so the mask folds away.
        const unsigned interested = hook == GuestResourceHook::EndTiling ? unsigned(kGuestResolveSurfaces) : ~0U;
        if (const auto published = guestResourceObservers.load(std::memory_order_relaxed) & interested) [[unlikely]]
        {
            this->hook = hook;
            context    = &ctx;
            observers  = published;
            switch (hook)
            {
                case GuestResourceHook::XgVertexBuffer:
                    argument = ctx.r7.u32;
                    break;
                case GuestResourceHook::XgIndexBuffer:
                    argument = ctx.r8.u32;
                    break;
                case GuestResourceHook::SetTexture:
                case GuestResourceHook::SetStreamSource:
                    argument = ctx.r5.u32;
                    break;
                case GuestResourceHook::SetIndices:
                    argument = ctx.r4.u32;
                    break;
                case GuestResourceHook::FreePhysical:
                    // Before Original releases the allocation and its extent.
                    argument = ctx.r3.u32;
                    extent   = GuestAllocationExtent(argument);
                    break;
                case GuestResourceHook::LockCore:
                    argument = ctx.r3.u32;
                    if (published & kGuestResolveSurfaces)
                        ObserveResolveSurfaceLock(argument, ctx.r1.u32);
                    break;
                default:
                    argument = ctx.r3.u32;
                    break;
            }
        }
    }

    ~GuestResourceScope()
    {
        if (!context)
            return;
        if (observers & kGuestResourceMirrors)
            ObserveGuestResourceCall(hook, argument, extent, *context);
        if (observers & kGuestResolveSurfaces)
            switch (hook)
            {
                case GuestResourceHook::UnlockCore:
                    ObserveResolveSurfaceUnlock(argument);
                    break;
                case GuestResourceHook::FreePhysical:
                    ObserveResolveSurfaceFree(argument, extent);
                    break;
                case GuestResourceHook::EndTiling:
                    ObserveResolveSurfaceTiling();
                    break;
                default:
                    break;
            }
    }

    GuestResourceScope(const GuestResourceScope&)            = delete;
    GuestResourceScope& operator=(const GuestResourceScope&) = delete;

private:
    GuestResourceHook hook      = GuestResourceHook::CreateTexture;
    const PPCContext* context   = nullptr;
    std::uint32_t     argument  = 0;
    std::uint32_t     extent    = 0;
    unsigned          observers = 0;
};

struct GuestResourceUnobserved
{
};

// Expands to a scope only for the entries above, so shared hook macros cost
// nothing for every other entry.
template <std::uint32_t Address>
[[nodiscard]] auto ObserveResourceMirror(const PPCContext& ctx) noexcept
{
    if constexpr (constexpr auto hook = FindGuestResourceHook(Address); hook.has_value())
        return GuestResourceScope(*hook, ctx);
    else
        return GuestResourceUnobserved{};
}

} // namespace rerevved::gpu::guest_d3d
