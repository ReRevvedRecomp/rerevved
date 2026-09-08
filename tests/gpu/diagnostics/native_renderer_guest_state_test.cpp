#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <cstdlib>
#include <iostream>

namespace
{

void require(bool condition, const char* message)
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

    require(PublishGuestDevice(0xFFCAE000, 0x40123450),
            "first publication changes state");

    require(!PublishGuestDevice(0xFFCAE000, 0x40123450),
            "duplicate publication is stable");
    require(PublishGuestDevice(0xFFCAE000, 0x40167890),
            "replacement publication changes state");

    require(ObserveGuestTexture(1024, 256) == 1,
            "first texture sequence");
    require(ObserveGuestTexture(64, 64) == 2,
            "second texture sequence");

    const GuestFetchDescriptor firstResolve = {
        0x11111111,
        0x34567007,
        0x22222222,
        0x33333333,
        0x44444444,
        0x55555555,
    };
    GuestFetchDescriptor secondResolve = firstResolve;
    secondResolve[0]                   = 0xAAAAAAAA;
    require(ObserveGuestResolve(
                firstResolve, 0x82512BA8, 0x100, 1, 2) == 1,
            "first resolve sequence");
    require(ObserveGuestResolve(
                secondResolve, 0x826A882C, 0x200, 3, 4) == 2,
            "second resolve sequence");

    GuestSwapCorrelation swap = ObserveGuestSwap(secondResolve);
    require(swap.match == GuestFetchMatch::Exact, "exact resolve match");
    require(swap.resolveSequence == 2, "latest exact resolve retained");
    require(swap.resolveCallAddress == 0x826A882C &&
                swap.resolveFlags == 0x200,
            "exact resolve context retained");
    require(swap.resolveMipLevel == 3 && swap.resolveSlice == 4,
            "exact resolve subresource retained");
    require(swap.swapSequence == 1 && swap.matchedCount == 1,
            "first matched swap counted");

    swap = ObserveGuestSwap(secondResolve);
    require(swap.match == GuestFetchMatch::None,
            "resolve before preceding swap is stale");

    GuestFetchDescriptor baseOnly = secondResolve;
    baseOnly[0]                   = 0xBBBBBBBB;
    baseOnly[2]                   = 0xCCCCCCCC;
    require(ObserveGuestResolve(
                secondResolve, 0x826A882C, 0x200, 3, 4) == 3,
            "third resolve sequence");
    swap = ObserveGuestSwap(baseOnly);
    require(swap.match == GuestFetchMatch::BaseAddressCandidate,
            "base-address resolve candidate");
    require(swap.resolveSequence == 3,
            "base candidate keeps resolve sequence");
    require(swap.swapSequence == 3 && swap.candidateCount == 1 &&
                swap.matchedCount == 1,
            "base candidate counted separately");

    GuestFetchDescriptor unmatched = baseOnly;
    unmatched[1]                   = 0x76543007;
    swap                           = ObserveGuestSwap(unmatched);
    require(swap.match == GuestFetchMatch::None, "unmatched swap rejected");
    require(swap.swapSequence == 4 && swap.unmatchedCount == 2,
            "unmatched swap counted");

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    ResetGuestSwapCorrelation();
    swap = ObserveGuestSwap(unmatched);
    require(swap.swapSequence == 1 && swap.unmatchedCount == 1,
            "swap correlation reset state");

    std::cout << "native_renderer_guest_state_test: PASS\n";
    return 0;
}
