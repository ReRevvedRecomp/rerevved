#pragma once

#include <atomic>

namespace rerevved::gpu::guest_d3d
{

// The registries the guest resource hooks (resource_mirror_hooks.h) feed,
// as bits of one word: a hook with none published pays one relaxed load.
enum GuestResourceObserver : unsigned
{
    kGuestResourceMirrors = 1,
    kGuestResolveSurfaces = 2,
};

inline std::atomic<unsigned> guestResourceObservers{ 0 };

inline void SetGuestResourceObserver(GuestResourceObserver observer, bool published) noexcept
{
    if (published)
        guestResourceObservers.fetch_or(observer, std::memory_order_relaxed);
    else
        guestResourceObservers.fetch_and(~unsigned(observer), std::memory_order_relaxed);
}

} // namespace rerevved::gpu::guest_d3d
