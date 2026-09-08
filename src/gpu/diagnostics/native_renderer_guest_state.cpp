#include "native_renderer_guest_state.h"

#include <atomic>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t   kResolveHistorySize = 64;
constexpr std::uint32_t kFetchBaseMask      = 0xFFFFF000u;

struct GuestResolveRecord
{
    GuestFetchDescriptor descriptor{};
    std::uint64_t        sequence    = 0;
    std::uint32_t        callAddress = 0;
    std::uint32_t        flags       = 0;
    std::uint32_t        mipLevel    = 0;
    std::uint32_t        slice       = 0;
};

struct GuestSwapState
{
    std::array<GuestResolveRecord, kResolveHistorySize> resolves{};
    std::uint64_t                                       resolveSequence             = 0;
    std::uint64_t                                       previousSwapResolveSequence = 0;
    std::uint64_t                                       swapSequence                = 0;
    std::uint64_t                                       matchedCount                = 0;
    std::uint64_t                                       candidateCount              = 0;
    std::uint64_t                                       unmatchedCount              = 0;
};

std::atomic_uint64_t guestDevicePublication = 0;
std::atomic_flag     guestStateLock         = ATOMIC_FLAG_INIT;
std::uint64_t        guestTextureSequence   = 0;
GuestSwapState       guestSwapState;

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

std::uint64_t ObserveGuestResolve(
    const GuestFetchDescriptor& descriptor,
    std::uint32_t               callAddress,
    std::uint32_t               flags,
    std::uint32_t               mipLevel,
    std::uint32_t               slice) noexcept
{
    lockGuestState();
    const std::uint64_t sequence = ++guestSwapState.resolveSequence;
    GuestResolveRecord& record =
        guestSwapState.resolves[(sequence - 1) % kResolveHistorySize];
    record = {
        descriptor,
        sequence,
        callAddress,
        flags,
        mipLevel,
        slice,
    };
    unlockGuestState();
    return sequence;
}

GuestSwapCorrelation ObserveGuestSwap(
    const GuestFetchDescriptor& descriptor) noexcept
{
    lockGuestState();

    GuestSwapCorrelation result{};
    result.swapSequence = ++guestSwapState.swapSequence;

    const auto findMatch = [&](bool exact)
    {
        const std::uint32_t wantedBase = descriptor[1] & kFetchBaseMask;
        const std::uint64_t firstRetained =
            guestSwapState.resolveSequence >= kResolveHistorySize
                ? guestSwapState.resolveSequence - kResolveHistorySize + 1
                : 1;
        const std::uint64_t firstAfterPreviousSwap =
            guestSwapState.previousSwapResolveSequence + 1;
        const std::uint64_t firstEligible =
            firstRetained > firstAfterPreviousSwap
                ? firstRetained
                : firstAfterPreviousSwap;
        const std::uint64_t retained =
            guestSwapState.resolveSequence >= firstEligible
                ? guestSwapState.resolveSequence - firstEligible + 1
                : 0;
        for (std::uint64_t age = 0; age < retained; ++age)
        {
            const std::uint64_t sequence =
                guestSwapState.resolveSequence - age;
            const GuestResolveRecord& record = guestSwapState.resolves[(sequence - 1) % kResolveHistorySize];
            if (record.sequence != sequence)
            {
                continue;
            }
            const bool matches = exact
                                     ? record.descriptor == descriptor
                                     : wantedBase != 0 &&
                                           (record.descriptor[1] &
                                            kFetchBaseMask) == wantedBase;
            if (matches)
            {
                result.resolveSequence    = record.sequence;
                result.resolveCallAddress = record.callAddress;
                result.resolveFlags       = record.flags;
                result.resolveMipLevel    = record.mipLevel;
                result.resolveSlice       = record.slice;
                return true;
            }
        }
        return false;
    };

    if (findMatch(true))
    {
        result.match = GuestFetchMatch::Exact;
    }
    else if (findMatch(false))
    {
        result.match = GuestFetchMatch::BaseAddressCandidate;
    }

    guestSwapState.previousSwapResolveSequence =
        guestSwapState.resolveSequence;
    if (result.match == GuestFetchMatch::None)
    {
        result.unmatchedCount = ++guestSwapState.unmatchedCount;
        result.matchedCount   = guestSwapState.matchedCount;
        result.candidateCount = guestSwapState.candidateCount;
    }
    else if (result.match == GuestFetchMatch::Exact)
    {
        result.matchedCount   = ++guestSwapState.matchedCount;
        result.candidateCount = guestSwapState.candidateCount;
        result.unmatchedCount = guestSwapState.unmatchedCount;
    }
    else
    {
        result.matchedCount   = guestSwapState.matchedCount;
        result.candidateCount = ++guestSwapState.candidateCount;
        result.unmatchedCount = guestSwapState.unmatchedCount;
    }

    unlockGuestState();
    return result;
}

void ResetGuestSwapCorrelation() noexcept
{
    lockGuestState();
    guestSwapState = {};
    unlockGuestState();
}

} // namespace rerevved::gpu
