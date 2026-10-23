#include "advisor_popups.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <string_view>

namespace
{

using namespace rerevved::advisor_popups;

void testTableShape()
{
    std::set<uint32_t> addresses;
    std::set<int>      groups;
    for (const Site& site : kSites)
    {
        UNSCOPED_INFO("each site appears once");
        CHECK(addresses.insert(site.address).second);
        UNSCOPED_INFO("site group is in range");
        CHECK(site.group < kGroupCvars.size());
        UNSCOPED_INFO("site is word aligned");
        CHECK(site.address % 4 == 0);
        groups.insert(site.group);
    }
    UNSCOPED_INFO("65 call sites");
    CHECK(addresses.size() == 65);
    UNSCOPED_INFO("every group has a site");
    CHECK(groups.size() == kGroupCvars.size());
}

void testLookupUsesReturnAddress()
{
    UNSCOPED_INFO("war news site maps to its group");
    CHECK(GroupForReturn(0x82CC89BC + 4) == 0);
    UNSCOPED_INFO("government prompt maps to its group");
    CHECK(GroupForReturn(0x82CEB298 + 4) == 14);
    UNSCOPED_INFO("the call address itself is not a return address");
    CHECK(GroupForReturn(0x82CC89BC) == -1);
    // The research prompt and the Special Functions menu keep native behavior.
    UNSCOPED_INFO("research prompt site is not grouped");
    CHECK(GroupForReturn(0x82CED0CC + 4) == -1);
    UNSCOPED_INFO("Special Functions menu is not grouped");
    CHECK(GroupForReturn(0x82D06534 + 4) == -1);
    UNSCOPED_INFO("unknown caller is not grouped");
    CHECK(GroupForReturn(0) == -1);
}

} // namespace

TEST_CASE("advisor_popups", "[ctest][advisor-popups]")
{
    testTableShape();
    testLookupUsesReturnAddress();
}
