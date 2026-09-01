#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rerevved::gpu
{

constexpr std::size_t kGuestFetchDwordCount = 6;
using GuestFetchDescriptor =
    std::array<std::uint32_t, kGuestFetchDwordCount>;

enum class GuestFetchMatch
{
    kNone,
    kBaseAddressCandidate,
    kExact,
};

struct GuestSwapCorrelation
{
    GuestFetchMatch match                = GuestFetchMatch::kNone;
    std::uint64_t   resolve_sequence     = 0;
    std::uint64_t   swap_sequence        = 0;
    std::uint64_t   matched_count        = 0;
    std::uint64_t   candidate_count      = 0;
    std::uint64_t   unmatched_count      = 0;
    std::uint32_t   resolve_call_address = 0;
    std::uint32_t   resolve_flags        = 0;
    std::uint32_t   resolve_mip_level    = 0;
    std::uint32_t   resolve_slice        = 0;
};

bool PublishGuestDevice(std::uint32_t cell_address,
                        std::uint32_t device_address) noexcept;
void ResetGuestDevicePublication() noexcept;

std::uint64_t ObserveGuestTexture(std::uint32_t width,
                                  std::uint32_t height) noexcept;
void          ResetGuestTextureObservation() noexcept;

std::uint64_t ObserveGuestResolve(
    const GuestFetchDescriptor& descriptor,
    std::uint32_t               call_address,
    std::uint32_t               flags,
    std::uint32_t               mip_level,
    std::uint32_t               slice) noexcept;
GuestSwapCorrelation ObserveGuestSwap(
    const GuestFetchDescriptor& descriptor) noexcept;
void ResetGuestSwapCorrelation() noexcept;

} // namespace rerevved::gpu
