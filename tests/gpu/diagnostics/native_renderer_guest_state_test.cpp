#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <cstdlib>
#include <iostream>

namespace
{

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "native_renderer_guest_state_test: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace rerevved::gpu;

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    ResetGuestSwapCorrelation();

    Require(PublishGuestDevice(0xFFCAE000, 0x40123450),
            "first publication changes state");

    Require(!PublishGuestDevice(0xFFCAE000, 0x40123450),
            "duplicate publication is stable");
    Require(PublishGuestDevice(0xFFCAE000, 0x40167890),
            "replacement publication changes state");

    Require(ObserveGuestTexture(1024, 256) == 1,
            "first texture sequence");
    Require(ObserveGuestTexture(64, 64) == 2,
            "second texture sequence");

    const GuestFetchDescriptor first_resolve = {
        0x11111111,
        0x34567007,
        0x22222222,
        0x33333333,
        0x44444444,
        0x55555555,
    };
    GuestFetchDescriptor second_resolve = first_resolve;
    second_resolve[0]                   = 0xAAAAAAAA;
    Require(ObserveGuestResolve(
                first_resolve, 0x82512BA8, 0x100, 1, 2) == 1,
            "first resolve sequence");
    Require(ObserveGuestResolve(
                second_resolve, 0x826A882C, 0x200, 3, 4) == 2,
            "second resolve sequence");

    GuestSwapCorrelation swap = ObserveGuestSwap(second_resolve);
    Require(swap.match == GuestFetchMatch::kExact, "exact resolve match");
    Require(swap.resolve_sequence == 2, "latest exact resolve retained");
    Require(swap.resolve_call_address == 0x826A882C &&
                swap.resolve_flags == 0x200,
            "exact resolve context retained");
    Require(swap.resolve_mip_level == 3 && swap.resolve_slice == 4,
            "exact resolve subresource retained");
    Require(swap.swap_sequence == 1 && swap.matched_count == 1,
            "first matched swap counted");

    swap = ObserveGuestSwap(second_resolve);
    Require(swap.match == GuestFetchMatch::kNone,
            "resolve before preceding swap is stale");

    GuestFetchDescriptor base_only = second_resolve;
    base_only[0]                   = 0xBBBBBBBB;
    base_only[2]                   = 0xCCCCCCCC;
    Require(ObserveGuestResolve(
                second_resolve, 0x826A882C, 0x200, 3, 4) == 3,
            "third resolve sequence");
    swap = ObserveGuestSwap(base_only);
    Require(swap.match == GuestFetchMatch::kBaseAddressCandidate,
            "base-address resolve candidate");
    Require(swap.resolve_sequence == 3,
            "base candidate keeps resolve sequence");
    Require(swap.swap_sequence == 3 && swap.candidate_count == 1 &&
                swap.matched_count == 1,
            "base candidate counted separately");

    GuestFetchDescriptor unmatched = base_only;
    unmatched[1]                   = 0x76543007;
    swap                           = ObserveGuestSwap(unmatched);
    Require(swap.match == GuestFetchMatch::kNone, "unmatched swap rejected");
    Require(swap.swap_sequence == 4 && swap.unmatched_count == 2,
            "unmatched swap counted");

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    ResetGuestSwapCorrelation();
    swap = ObserveGuestSwap(unmatched);
    Require(swap.swap_sequence == 1 && swap.unmatched_count == 1,
            "swap correlation reset state");

    std::cout << "native_renderer_guest_state_test: PASS\n";
    return 0;
}
