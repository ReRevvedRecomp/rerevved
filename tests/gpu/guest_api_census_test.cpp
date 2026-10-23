#include "gpu/guest_d3d/guest_api_census.h"
#include "gpu/guest_d3d/native_draw_hook.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace
{

using namespace rerevved::gpu::guest_d3d;

bool hasRole(std::uint32_t address, GuestApiRole role)
{
    const auto entry = FindGuestApiEntry(address);
    return entry && entry->role == role;
}

void tableHoldsKnownAnchors()
{
    std::set<std::uint32_t>    addresses;
    std::set<std::string_view> names;
    for (const auto& entry : kGuestApiEntries)
    {
        UNSCOPED_INFO("census addresses are unique");
        CHECK(addresses.insert(entry.address).second);
        UNSCOPED_INFO("census names are unique");
        CHECK(names.insert(entry.name).second);
        UNSCOPED_INFO("census entries are named");
        CHECK(!entry.name.empty());
    }
    UNSCOPED_INFO("D3DDevice_Resolve is a resolve");
    CHECK(hasRole(0x8269E520, GuestApiRole::Resolve));
    UNSCOPED_INFO("the VdSwap owner is the present");
    CHECK(hasRole(0x826A4638, GuestApiRole::Present));
    UNSCOPED_INFO("device publication is a device entry");
    CHECK(hasRole(0x826A63E8, GuestApiRole::Device));
    UNSCOPED_INFO("ring reservation is ring internal");
    CHECK(hasRole(0x8269C7A8, GuestApiRole::RingInternal));
    UNSCOPED_INFO("ring initialization is ring internal");
    CHECK(hasRole(0x8269C8E0, GuestApiRole::RingInternal));
    for (const auto address : kNativeDrawHookAddresses)
    {
        UNSCOPED_INFO("every native draw hook is a census draw");
        CHECK(hasRole(address, GuestApiRole::Draw));
    }
    UNSCOPED_INFO("the runtime surface's SetTexture is seeded");
    CHECK(hasRole(0x8269AF10, GuestApiRole::SetTexture));
    UNSCOPED_INFO("render-state table setters are seeded");
    CHECK(hasRole(0x8268DDB8, GuestApiRole::SetRenderState));
    UNSCOPED_INFO("sampler-state table setters are seeded");
    CHECK(hasRole(0x8268F140, GuestApiRole::SetSamplerState));
    UNSCOPED_INFO("the draw commit is not a census entry");
    CHECK(!FindGuestApiEntry(kNativeDrawCommitAddress));
}

// One hooked guest call: the scope a hook holds while Original runs.
template <std::uint32_t Address, typename Nested = void (*)()>
void call(Nested nested = []
          {
          })
{
    [[maybe_unused]] const auto scope = CountGuestApiCall<Address>();
    nested();
}

void disabledCensusCountsNothing()
{
    EnableGuestApiCensus(false);
    call<0x8269E520>();
    call<0x826A4638>();
    const auto counts = TakeGuestApiCounts();
    for (const auto count : counts)
    {
        UNSCOPED_INFO("a disabled census counts nothing");
        CHECK(count == 0);
    }
    UNSCOPED_INFO("zero counts render no fields");
    CHECK(FormatGuestApiCounts(counts).empty());
}

void enabledCensusCountsByRole()
{
    EnableGuestApiCensus(true);
    call<0x8269E520>();
    call<0x8269E520>();
    call<0x826A4638>();
    call<0x826A39F8>();
    call<0x826A3568>([]
                     {
                         call<0x826A3048>();
                     }); // IndexedUp's own lowerer call
    call<0x826A3048>();  // a direct lowerer draw
    call<0x8269D320>([]
                     {
                         call<0x8269E520>();
                     }); // EndTiling's per-tile resolve
    call<0x8268CE90>([]
                     {
                         call<0x8269C8E0>();
                     }); // a reset reinitializes the ring
    call<0x826A2FF0>();  // outside the table
    const auto counts = TakeGuestApiCounts();
    UNSCOPED_INFO("a draw inside a draw counts once");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::Draw)] == 3);
    UNSCOPED_INFO("a resolve inside a resolve counts once");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::Resolve)] == 3);
    UNSCOPED_INFO("presents count per call");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::Present)] == 1);
    UNSCOPED_INFO("device calls count per call");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::Device)] == 1);
    UNSCOPED_INFO("a call inside another role counts");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::RingInternal)] == 1);
    const auto fields = FormatGuestApiCounts(counts);
    UNSCOPED_INFO("fields render nonzero roles in role order: " + fields);
    CHECK(fields == " api_device=1 api_draw=3 api_resolve=3 api_present=1 api_ring_internal=1");

    const auto reset = TakeGuestApiCounts();
    UNSCOPED_INFO("a take resets every role");
    CHECK(FormatGuestApiCounts(reset).empty());
    EnableGuestApiCensus(false);
}

void disabledScopeLeavesNoDepth()
{
    EnableGuestApiCensus(false);
    {
        [[maybe_unused]] const auto outer = CountGuestApiCall<0x826A39F8>();
        EnableGuestApiCensus(true);
        call<0x826A3048>(); // the outer call held no depth, so this one counts
    }
    call<0x826A39F8>();
    const auto counts = TakeGuestApiCounts();
    UNSCOPED_INFO("enabling mid-call keeps depths balanced");
    CHECK(counts[static_cast<std::size_t>(GuestApiRole::Draw)] == 2);
    EnableGuestApiCensus(false);
}

void everyRoleHasAField()
{
    GuestApiCounts counts{};
    counts[static_cast<std::size_t>(GuestApiRole::Device)]     = 1;
    counts[static_cast<std::size_t>(GuestApiRole::SetTexture)] = 40;
    counts[static_cast<std::size_t>(GuestApiRole::Other)]      = 7;
    const auto fields                                          = FormatGuestApiCounts(counts);
    UNSCOPED_INFO("fields name each role: " + fields);
    CHECK(fields == " api_device=1 api_set_texture=40 api_other=7");
    std::set<std::string_view> names(kGuestApiRoleNames.begin(), kGuestApiRoleNames.end());
    UNSCOPED_INFO("role field names are unique");
    CHECK(names.size() == kGuestApiRoleCount);
}

} // namespace

TEST_CASE("guest_api_census", "[ctest][guest-api-census]")
{
    tableHoldsKnownAnchors();
    disabledCensusCountsNothing();
    enabledCensusCountsByRole();
    disabledScopeLeavesNoDepth();
    everyRoleHasAField();
}
