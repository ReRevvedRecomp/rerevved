#include "gpu/guest_d3d/guest_memory.h"

#include <cstring>
#include <stdexcept>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace rerevved::gpu::guest_d3d
{

std::span<const std::uint8_t> ViewGuestMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    const auto ceiling = physical ? 0x20000000ULL : 0x100000000ULL;
    if (!size || size > 16 * 1024 * 1024 || address >= ceiling || size > ceiling - address)
        throw std::runtime_error("guest_memory_extent");
    auto*      memory = REX_KERNEL_MEMORY();
    auto*      heap   = physical ? memory->GetPhysicalHeap() : memory->LookupHeap(static_cast<std::uint32_t>(address));
    const auto last   = static_cast<std::uint32_t>(address + size - 1);
    if (!heap || (!physical && memory->LookupHeap(last) != heap) ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(static_cast<std::uint32_t>(address), last)) &
          static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        throw std::runtime_error("guest_memory_uncommitted");
    const auto* data = physical ? memory->TranslatePhysical<const std::uint8_t*>(static_cast<std::uint32_t>(address)) : memory->TranslateVirtual<const std::uint8_t*>(static_cast<std::uint32_t>(address));
    return { data, static_cast<std::size_t>(size) };
}

std::vector<std::uint8_t> ReadGuestMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    const auto bytes = ViewGuestMemory(address, size, physical);
    return { bytes.begin(), bytes.end() };
}

namespace
{

void* copyThunk(void* destination, const void* source, std::size_t size)
{
    return std::memcpy(destination, source, size);
}

} // namespace

// A page can go away after the access query. The copy is called through a volatile pointer so it
// stays a call that the exception scope covers whatever the optimizer does with a known size: an
// inlined load is not in the scope table without /EHa.
bool CopyGuestGuardedWith(GuestCopyFn copy, void* destination, const void* source, std::size_t size)
{
    volatile GuestCopyFn call = copy;
#if defined(_WIN32)
    __try
    {
        call(destination, source, size);
        return true;
    }
    __except (_exception_code() == 0xC0000005 || _exception_code() == 0xC0000006 ? 1 : 0)
    {
        return false;
    }
#else
    call(destination, source, size);
    return true;
#endif
}

bool CopyGuestGuarded(void* destination, const void* source, std::size_t size) noexcept
{
    return CopyGuestGuardedWith(&copyThunk, destination, source, size);
}

} // namespace rerevved::gpu::guest_d3d
