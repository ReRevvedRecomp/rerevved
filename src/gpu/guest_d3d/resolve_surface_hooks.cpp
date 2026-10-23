#include "resolve_surface_hooks.h"

#include "gpu/guest_d3d/resource_mirror_registry.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>

namespace rerevved::gpu::guest_d3d
{
namespace
{

// Guest bytes at a virtual address, or false when the pages are not readable.
bool readGuest(std::uint32_t address, std::span<std::uint8_t> bytes)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address || address > UINT32_MAX - bytes.size())
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + static_cast<std::uint32_t>(bytes.size() - 1);
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    std::memcpy(bytes.data(), memory->TranslateVirtual<const std::uint8_t*>(address), bytes.size());
    return true;
}

std::uint32_t readWord(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

} // namespace

void ObserveResolveSurfaceLock(ResolveSurfaceRegistry& surfaces, std::uint32_t object, std::uint32_t stack, const ResolveSurfaceGuestRead& read)
{
    std::array<std::uint8_t, 4>                        flagBytes{};
    std::array<std::uint8_t, kGuestTextureHeaderBytes> header{};
    if (!read(stack + 84, flagBytes) || !read(object, std::span(header).first(4)))
        return;
    const auto flags = readWord(flagBytes, 0);
    // The common type picks the kind, as the lock core picks its fence
    // object: a level (type 4 with bit 30) names its texture at +0x18.
    auto       described = object;
    const auto common    = readWord(header, 0);
    auto       kind      = GuestResourceKind::Texture;
    switch (common & 0xF)
    {
        case 1:
            kind = GuestResourceKind::VertexBuffer;
            break;
        case 2:
            kind = GuestResourceKind::IndexBuffer;
            break;
        case 4:
            if (!(common & 0x40000000U) || !read(object, std::span(header).first(kGuestSurfaceHeaderBytes)) || !(described = readWord(header, 0x18)))
                return;
            break;
        default:
            break;
    }
    const auto                bytes = std::span(header).first(GuestResourceHeaderBytes(kind));
    GuestResourceRegistration registration;
    if (!read(described, bytes) || !DescribeGuestResource(kind, described, bytes, registration))
        return;
    ResolveSurfaceLockRanges ranges{};
    for (std::size_t i = 0; i < ranges.size(); ++i)
        ranges[i] = { registration.ranges[i].address, std::uint64_t(registration.ranges[i].address) + registration.ranges[i].size };
    // A queued draw binds the entries as they were before the lock.
    if (surfaces.Touches(ranges))
        surfaces.Barrier();
    // Registered first, so a take from here on writes its image now; then
    // the held images the lock names land before the guest reads them.
    surfaces.Locked(described, ranges, !(flags & 0x10));
    surfaces.Settle(&ranges);
}

void ObserveResolveSurfaceLock(std::uint32_t object, std::uint32_t stack) noexcept
{
    try
    {
        if (const auto surfaces = PublishedResolveSurfaces())
            ObserveResolveSurfaceLock(*surfaces, object, stack, readGuest);
    }
    catch (const std::exception&)
    {
        // A lock the registry misses leaves its entries to the write watch.
    }
}

void ObserveResolveSurfaceUnlock(std::uint32_t object) noexcept
{
    try
    {
        if (const auto surfaces = PublishedResolveSurfaces())
        {
            if (surfaces->UnlockTouches(object))
                surfaces->Barrier();
            surfaces->Unlocked(object);
            surfaces->Settle();
        }
    }
    catch (const std::exception&)
    {
    }
}

void ObserveResolveSurfaceTiling() noexcept
{
    try
    {
        if (const auto surfaces = PublishedResolveSurfaces())
        {
            // A queued draw from inside the bracket binds the version before it.
            if (surfaces->TilesPending())
                surfaces->Barrier();
            surfaces->CommitTiles();
            surfaces->Settle();
        }
    }
    catch (const std::exception&)
    {
    }
}

void ObserveResolveSurfaceFree(std::uint32_t address, std::uint32_t extent) noexcept
{
    try
    {
        // Without the heap's extent the free still ends what starts there.
        if (const auto surfaces = PublishedResolveSurfaces())
        {
            const auto                     begin = GuestPhysicalAddress(address);
            const ResolveSurfaceLockRanges freed{ { { begin, std::uint64_t(begin) + std::max<std::uint32_t>(extent, 1) }, {} } };
            if (surfaces->Touches(freed))
                surfaces->Barrier();
            surfaces->Freed(freed[0].first, freed[0].second);
            surfaces->Settle();
        }
    }
    catch (const std::exception&)
    {
    }
}

} // namespace rerevved::gpu::guest_d3d
