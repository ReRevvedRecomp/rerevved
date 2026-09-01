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
    std::uint64_t        sequence     = 0;
    std::uint32_t        call_address = 0;
    std::uint32_t        flags        = 0;
    std::uint32_t        mip_level    = 0;
    std::uint32_t        slice        = 0;
};

struct GuestSwapState
{
    std::array<GuestResolveRecord, kResolveHistorySize> resolves{};
    std::uint64_t                                       resolve_sequence               = 0;
    std::uint64_t                                       previous_swap_resolve_sequence = 0;
    std::uint64_t                                       swap_sequence                  = 0;
    std::uint64_t                                       matched_count                  = 0;
    std::uint64_t                                       candidate_count                = 0;
    std::uint64_t                                       unmatched_count                = 0;
};

std::atomic_uint64_t guest_device_publication = 0;
std::atomic_flag     guest_state_lock         = ATOMIC_FLAG_INIT;
std::uint64_t        guest_texture_sequence   = 0;
GuestSwapState       guest_swap_state;

std::uint64_t Pack(std::uint32_t cell_address,
                   std::uint32_t device_address) noexcept
{
    return (std::uint64_t{ cell_address } << 32) | device_address;
}

void LockGuestState() noexcept
{
    while (guest_state_lock.test_and_set(std::memory_order_acquire))
    {
    }
}

void UnlockGuestState() noexcept
{
    guest_state_lock.clear(std::memory_order_release);
}

} // namespace

bool PublishGuestDevice(std::uint32_t cell_address,
                        std::uint32_t device_address) noexcept
{
    const std::uint64_t publication = Pack(cell_address, device_address);
    return guest_device_publication.exchange(
               publication, std::memory_order_acq_rel) != publication;
}

void ResetGuestDevicePublication() noexcept
{
    guest_device_publication.store(0, std::memory_order_release);
}

std::uint64_t ObserveGuestTexture(std::uint32_t width,
                                  std::uint32_t height) noexcept
{
    LockGuestState();
    (void)width;
    (void)height;
    const std::uint64_t sequence = ++guest_texture_sequence;
    UnlockGuestState();
    return sequence;
}

void ResetGuestTextureObservation() noexcept
{
    LockGuestState();
    guest_texture_sequence = 0;
    UnlockGuestState();
}

std::uint64_t ObserveGuestResolve(
    const GuestFetchDescriptor& descriptor,
    std::uint32_t               call_address,
    std::uint32_t               flags,
    std::uint32_t               mip_level,
    std::uint32_t               slice) noexcept
{
    LockGuestState();
    const std::uint64_t sequence = ++guest_swap_state.resolve_sequence;
    GuestResolveRecord& record =
        guest_swap_state.resolves[(sequence - 1) % kResolveHistorySize];
    record = {
        descriptor,
        sequence,
        call_address,
        flags,
        mip_level,
        slice,
    };
    UnlockGuestState();
    return sequence;
}

GuestSwapCorrelation ObserveGuestSwap(
    const GuestFetchDescriptor& descriptor) noexcept
{
    LockGuestState();

    GuestSwapCorrelation result{};
    result.swap_sequence = ++guest_swap_state.swap_sequence;

    const auto find_match = [&](bool exact)
    {
        const std::uint32_t wanted_base = descriptor[1] & kFetchBaseMask;
        const std::uint64_t first_retained =
            guest_swap_state.resolve_sequence >= kResolveHistorySize
                ? guest_swap_state.resolve_sequence - kResolveHistorySize + 1
                : 1;
        const std::uint64_t first_after_previous_swap =
            guest_swap_state.previous_swap_resolve_sequence + 1;
        const std::uint64_t first_eligible =
            first_retained > first_after_previous_swap
                ? first_retained
                : first_after_previous_swap;
        const std::uint64_t retained =
            guest_swap_state.resolve_sequence >= first_eligible
                ? guest_swap_state.resolve_sequence - first_eligible + 1
                : 0;
        for (std::uint64_t age = 0; age < retained; ++age)
        {
            const std::uint64_t sequence =
                guest_swap_state.resolve_sequence - age;
            const GuestResolveRecord& record = guest_swap_state.resolves[(sequence - 1) % kResolveHistorySize];
            if (record.sequence != sequence)
            {
                continue;
            }
            const bool matches = exact
                                     ? record.descriptor == descriptor
                                     : wanted_base != 0 &&
                                           (record.descriptor[1] &
                                            kFetchBaseMask) == wanted_base;
            if (matches)
            {
                result.resolve_sequence     = record.sequence;
                result.resolve_call_address = record.call_address;
                result.resolve_flags        = record.flags;
                result.resolve_mip_level    = record.mip_level;
                result.resolve_slice        = record.slice;
                return true;
            }
        }
        return false;
    };

    if (find_match(true))
    {
        result.match = GuestFetchMatch::kExact;
    }
    else if (find_match(false))
    {
        result.match = GuestFetchMatch::kBaseAddressCandidate;
    }

    guest_swap_state.previous_swap_resolve_sequence =
        guest_swap_state.resolve_sequence;
    if (result.match == GuestFetchMatch::kNone)
    {
        result.unmatched_count = ++guest_swap_state.unmatched_count;
        result.matched_count   = guest_swap_state.matched_count;
        result.candidate_count = guest_swap_state.candidate_count;
    }
    else if (result.match == GuestFetchMatch::kExact)
    {
        result.matched_count   = ++guest_swap_state.matched_count;
        result.candidate_count = guest_swap_state.candidate_count;
        result.unmatched_count = guest_swap_state.unmatched_count;
    }
    else
    {
        result.matched_count   = guest_swap_state.matched_count;
        result.candidate_count = ++guest_swap_state.candidate_count;
        result.unmatched_count = guest_swap_state.unmatched_count;
    }

    UnlockGuestState();
    return result;
}

void ResetGuestSwapCorrelation() noexcept
{
    LockGuestState();
    guest_swap_state = {};
    UnlockGuestState();
}

} // namespace rerevved::gpu
