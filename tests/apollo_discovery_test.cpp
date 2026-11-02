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

TEST_CASE("apollo_discovery_drains_only_when_nothing_native_is_pending", "[ctest][apollo-discovery]")
{
    CHECK(ad::DrainReady({}));
    CHECK(ad::DrainReady({ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0 }));
    CHECK_FALSE(ad::DrainReady({ 3, 0, 0, 0 }));
    CHECK_FALSE(ad::DrainReady({ 0, 12, 0, 0 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0, 4, 0 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0, 0, 0x1000 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0, 0, 0x1100 }));
    CHECK_FALSE(ad::DrainReady({ 0, 0, 0, 0x0100 }));
    CHECK(ad::DrainReady({ 0, 0, 0, 0xFFFFEEFFu }));
}
