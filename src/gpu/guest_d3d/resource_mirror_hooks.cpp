#include "resource_mirror_hooks.h"

#include <rex/hook.h>
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

// A guest object's header bytes, or false when the pages are not readable.
bool readHeader(std::uint32_t address, std::span<std::uint8_t> bytes)
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

void registerObject(GuestResourceMirrors& mirrors, GuestResourceKind kind, std::uint32_t header)
{
    std::array<std::uint8_t, kGuestTextureHeaderBytes> storage{};
    const auto                                         bytes = std::span(storage).first(GuestResourceHeaderBytes(kind));
    GuestResourceRegistration                          registration;
    if (readHeader(header, bytes) && DescribeGuestResource(kind, header, bytes, registration))
        mirrors.Register(registration);
}

// A bound header the registry does not hold with these words yet: one the
// create hooks missed, or one whose storage changed since.
void registerBound(GuestResourceMirrors& mirrors, GuestResourceKind kind, std::uint32_t header)
{
    std::array<std::uint8_t, kGuestTextureHeaderBytes> storage{};
    const auto                                         bytes = std::span(storage).first(GuestResourceHeaderBytes(kind));
    std::array<std::uint32_t, 6>                       words{};
    if (!header || !readHeader(header, bytes) || !GuestResourceWords(kind, bytes, words) || mirrors.Holds(kind, header, words))
        return;
    GuestResourceRegistration registration;
    if (DescribeGuestResource(kind, header, bytes, registration))
        mirrors.Register(registration, true);
}

// The lock core's object: its kind by common type, and the texture a level
// names at +0x18 (type 4 with bit 30, as 0x82697CA0 picks its fence object).
void observeLock(GuestResourceMirrors& mirrors, std::uint32_t header)
{
    std::array<std::uint8_t, kGuestSurfaceHeaderBytes> bytes{};
    if (!readHeader(header, std::span(bytes).first(4)))
        return;
    const auto common = readWord(bytes, 0);
    auto       kind   = GuestResourceKind::Texture;
    switch (common & 0xF)
    {
        case 1:
            kind = GuestResourceKind::VertexBuffer;
            break;
        case 2:
            kind = GuestResourceKind::IndexBuffer;
            break;
        case 4:
            kind = GuestResourceKind::Surface;
            if ((common & 0x40000000U) && readHeader(header, bytes))
                if (const auto parent = readWord(bytes, 0x18))
                    header = parent;
            break;
        default:
            break;
    }
    mirrors.Locked(kind, header);
}

} // namespace

std::uint32_t GuestAllocationExtent(std::uint32_t address) noexcept
{
    // MmFreePhysicalMemory releases the allocation at this address in its
    // heap, which invalidates QuerySize bytes from it.
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address)
        return 0;
    auto*         heap = kernel->memory()->LookupHeap(address);
    std::uint32_t size = 0;
    if (!heap || !heap->QuerySize(address, &size))
        return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(size, std::uint64_t(UINT32_MAX) - GuestPhysicalAddress(address)));
}

void ObserveGuestResourceCall(GuestResourceHook hook, std::uint32_t argument, std::uint32_t extent, const PPCContext& ctx) noexcept
{
    try
    {
        const auto mirrors = PublishedGuestResourceMirrors();
        if (!mirrors)
            return;
        const auto result = ctx.r3.u32;
        switch (hook)
        {
            case GuestResourceHook::CreateTexture:
                if (result)
                    registerObject(*mirrors, GuestResourceKind::Texture, result);
                break;
            case GuestResourceHook::CreateVertexBuffer:
                if (result)
                    registerObject(*mirrors, GuestResourceKind::VertexBuffer, result);
                break;
            case GuestResourceHook::CreateIndexBuffer:
                if (result)
                    registerObject(*mirrors, GuestResourceKind::IndexBuffer, result);
                break;
            case GuestResourceHook::XgVertexBuffer:
                registerObject(*mirrors, GuestResourceKind::VertexBuffer, argument);
                break;
            case GuestResourceHook::XgIndexBuffer:
                registerObject(*mirrors, GuestResourceKind::IndexBuffer, argument);
                break;
            case GuestResourceHook::PatchTextureBase:
                registerObject(*mirrors, GuestResourceKind::Texture, argument);
                break;
            case GuestResourceHook::PatchResourceBase:
            {
                // Common type 1 is a vertex buffer and 2 an index buffer
                // (0x826982A8, 0x826983D0); textures patch through 0x826B7140.
                std::array<std::uint8_t, 4> common{};
                if (!readHeader(argument, common))
                    break;
                const auto type = common[3] & 0xF;
                if (type == 1 || type == 2)
                    registerObject(*mirrors, type == 1 ? GuestResourceKind::VertexBuffer : GuestResourceKind::IndexBuffer, argument);
                break;
            }
            case GuestResourceHook::SetTexture:
                registerBound(*mirrors, GuestResourceKind::Texture, argument);
                break;
            case GuestResourceHook::SetStreamSource:
                registerBound(*mirrors, GuestResourceKind::VertexBuffer, argument);
                break;
            case GuestResourceHook::SetIndices:
                registerBound(*mirrors, GuestResourceKind::IndexBuffer, argument);
                break;
            case GuestResourceHook::LockCore:
                observeLock(*mirrors, argument);
                break;
            case GuestResourceHook::UnlockCore:
                mirrors->Unlocked(argument);
                break;
            case GuestResourceHook::ReleaseResource:
                if (!result)
                    mirrors->Released(argument);
                break;
            case GuestResourceHook::ReleaseDevice:
                if (!result)
                    mirrors->ReleasedAll();
                break;
            case GuestResourceHook::FreePhysical:
                mirrors->Freed(GuestPhysicalAddress(argument), extent);
                break;
            case GuestResourceHook::EndTiling:
                break;
        }
    }
    catch (const std::exception&)
    {
        // Registration allocates; an object it misses binds through the bytes path.
    }
}

} // namespace rerevved::gpu::guest_d3d

// Only the title executable links generated originals. Every unlock entry
// reaches the unlock core and every backing free reaches 0x8280DEC8; no other
// hook wraps either.
#if defined(REREVVED_VERSION)
REX_EXTERN(__imp__sub_82697F38);
REX_EXTERN(__imp__sub_8280DEC8);

REX_HOOK_RAW(sub_82697F38)
{
    [[maybe_unused]] const auto resourceMirror = rerevved::gpu::guest_d3d::ObserveResourceMirror<0x82697F38>(ctx);
    __imp__sub_82697F38(ctx, base);
}

REX_HOOK_RAW(sub_8280DEC8)
{
    [[maybe_unused]] const auto resourceMirror = rerevved::gpu::guest_d3d::ObserveResourceMirror<0x8280DEC8>(ctx);
    __imp__sub_8280DEC8(ctx, base);
}
#endif
