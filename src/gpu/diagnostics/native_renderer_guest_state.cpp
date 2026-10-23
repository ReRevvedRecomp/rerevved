#include "native_renderer_guest_state.h"

#include <atomic>

namespace rerevved::gpu
{
namespace
{

std::atomic_uint64_t guestDevicePublication = 0;
std::atomic_flag     guestStateLock         = ATOMIC_FLAG_INIT;
std::uint64_t        guestTextureSequence   = 0;

std::uint64_t pack(std::uint32_t cellAddress,
                   std::uint32_t deviceAddress) noexcept
{
    return (std::uint64_t{ cellAddress } << 32) | deviceAddress;
}

void lockGuestState() noexcept
{
    while (guestStateLock.test_and_set(std::memory_order_acquire))
    {
    }
}

void unlockGuestState() noexcept
{
    guestStateLock.clear(std::memory_order_release);
}

} // namespace

bool PublishGuestDevice(std::uint32_t cellAddress,
                        std::uint32_t deviceAddress) noexcept
{
    const std::uint64_t publication = pack(cellAddress, deviceAddress);
    return guestDevicePublication.exchange(
               publication, std::memory_order_acq_rel) != publication;
}

void ResetGuestDevicePublication() noexcept
{
    guestDevicePublication.store(0, std::memory_order_release);
}

std::uint64_t ObserveGuestTexture(std::uint32_t width,
                                  std::uint32_t height) noexcept
{
    lockGuestState();
    (void)width;
    (void)height;
    const std::uint64_t sequence = ++guestTextureSequence;
    unlockGuestState();
    return sequence;
}

void ResetGuestTextureObservation() noexcept
{
    lockGuestState();
    guestTextureSequence = 0;
    unlockGuestState();
}

} // namespace rerevved::gpu
