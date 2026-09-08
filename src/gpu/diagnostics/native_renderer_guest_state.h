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
    None,
    BaseAddressCandidate,
    Exact,
};

struct GuestSwapCorrelation
{
    GuestFetchMatch match              = GuestFetchMatch::None;
    std::uint64_t   resolveSequence    = 0;
    std::uint64_t   swapSequence       = 0;
    std::uint64_t   matchedCount       = 0;
    std::uint64_t   candidateCount     = 0;
    std::uint64_t   unmatchedCount     = 0;
    std::uint32_t   resolveCallAddress = 0;
    std::uint32_t   resolveFlags       = 0;
    std::uint32_t   resolveMipLevel    = 0;
    std::uint32_t   resolveSlice       = 0;
};

bool PublishGuestDevice(std::uint32_t cellAddress,
                        std::uint32_t deviceAddress) noexcept;
void ResetGuestDevicePublication() noexcept;

std::uint64_t ObserveGuestTexture(std::uint32_t width,
                                  std::uint32_t height) noexcept;
void          ResetGuestTextureObservation() noexcept;

std::uint64_t ObserveGuestResolve(
    const GuestFetchDescriptor& descriptor,
    std::uint32_t               callAddress,
    std::uint32_t               flags,
    std::uint32_t               mipLevel,
    std::uint32_t               slice) noexcept;
GuestSwapCorrelation ObserveGuestSwap(
    const GuestFetchDescriptor& descriptor) noexcept;
void ResetGuestSwapCorrelation() noexcept;

} // namespace rerevved::gpu
