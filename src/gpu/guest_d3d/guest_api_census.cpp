#include "guest_api_census.h"

namespace rerevved::gpu::guest_d3d
{

std::atomic<bool>                                          guestApiCensusEnabled{ false };
std::array<std::atomic<std::uint64_t>, kGuestApiRoleCount> guestApiCensusCalls{};

void EnableGuestApiCensus(bool enabled) noexcept
{
    guestApiCensusEnabled.store(enabled, std::memory_order_relaxed);
}

GuestApiCounts TakeGuestApiCounts() noexcept
{
    GuestApiCounts counts{};
    for (std::size_t role = 0; role < kGuestApiRoleCount; ++role)
        counts[role] = guestApiCensusCalls[role].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatGuestApiCounts(const GuestApiCounts& counts)
{
    std::string fields;
    for (std::size_t role = 0; role < kGuestApiRoleCount; ++role)
    {
        if (!counts[role])
            continue;
        fields += " api_";
        fields += kGuestApiRoleNames[role];
        fields += '=';
        fields += std::to_string(counts[role]);
    }
    return fields;
}

} // namespace rerevved::gpu::guest_d3d
