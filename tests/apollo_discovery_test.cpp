#include "apollo_discovery.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

namespace ad = rerevved::apollo_discovery;

TEST_CASE("apollo_discovery_grants_only_whole_zero_words_in_ascending_order", "[ctest][apollo-discovery]")
{
    ad::OwnershipWords words{};
    CHECK(ad::EligibleTechnologies(words).size() == 46);
    CHECK(ad::EligibleTechnologies(words).front() == 1);
    CHECK(ad::EligibleTechnologies(words).back() == 46);

    // Any civilization's bit claims the word, technologies 0 and 47 stay unlisted.
    words[0]            = 0;
    words[3]            = 1u << 2;
    words[4]            = 1u << 7;
    words[10]           = 0x80000000u;
    words[46]           = 1;
    words[47]           = 0;
    const auto eligible = ad::EligibleTechnologies(words);
    CHECK(eligible.size() == 42);
    for (const int32_t technology : eligible)
    {
        CHECK(technology >= 1);
        CHECK(technology <= 46);
        CHECK(technology != 3);
        CHECK(technology != 4);
        CHECK(technology != 10);
        CHECK(technology != 46);
    }
    CHECK(eligible.front() == 1);
    CHECK(eligible[2] == 5);
    CHECK(ad::IsUnclaimed(0));
    CHECK_FALSE(ad::IsUnclaimed(1));
    CHECK_FALSE(ad::IsUnclaimed(0xFFFFFFFFu));

    words.fill(1);
    CHECK(ad::EligibleTechnologies(words).empty());
}

TEST_CASE("apollo_discovery_rebuilds_a_grant_the_loaded_world_still_owes", "[ctest][apollo-discovery]")
{
    ad::OwnershipWords words{};
    words.fill(1);
    words[9] = 0;
    CHECK(ad::GrantOwed(2, 40, 2, words));
    CHECK(ad::GrantOwed(0, 0, 0, words));
    CHECK(ad::GrantOwed(7, 127, 7, words));

    // Unclaimed, out of range, or the city changed hands.
    CHECK_FALSE(ad::GrantOwed(-1, -1, -1, words));
    CHECK_FALSE(ad::GrantOwed(8, 40, 8, words));
    CHECK_FALSE(ad::GrantOwed(2, 128, 2, words));
    CHECK_FALSE(ad::GrantOwed(2, -1, 2, words));
    CHECK_FALSE(ad::GrantOwed(2, 40, 3, words));
    CHECK_FALSE(ad::GrantOwed(2, 40, -1, words));

    // A finished grant leaves no unowned technology in 1..46; 0 and 47 do not count.
    words[9]  = 1;
    words[0]  = 0;
    words[47] = 0;
    CHECK_FALSE(ad::GrantOwed(2, 40, 2, words));
}

TEST_CASE("apollo_discovery_drains_only_when_nothing_native_is_pending", "[ctest][apollo-discovery]")
{
    CHECK(ad::DrainReady({}));
    CHECK(ad::DrainReady({ 0xFFFFFFFFu, 0 }));
    CHECK_FALSE(ad::DrainReady({ 4, 0 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0x1000 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0x1100 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0x0100 }));
    CHECK(ad::DrainReady({ 0, 0xFFFFEEFFu }));
}
