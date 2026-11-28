#include "retail_dlc.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace
{

rerevved::retail_dlc::Candidate candidate(std::string file, std::string token, std::vector<uint32_t> slots, std::vector<int32_t> ids)
{
    rerevved::retail_dlc::Candidate result{ std::move(file), std::move(token), {} };
    result.contents.slots = std::move(slots);
    for (const auto id : ids)
    {
        result.contents.scenarios.push_back({ id, "M" + std::to_string(id) });
    }
    return result;
}

} // namespace

TEST_CASE("retail DLC tokens are one printable word", "[ctest][retail-dlc]")
{
    using rerevved::retail_dlc::ParseToken;
    CHECK(ParseToken("victory\r\n") == "victory");
    CHECK(ParseToken("  scenario2") == "scenario2");
    CHECK(ParseToken("").empty());
    CHECK(ParseToken(" \r\n").empty());
    CHECK(ParseToken("two words").empty());
    CHECK(ParseToken(std::string("bad\0", 4)).empty());
}

TEST_CASE("retail DLC file names fit XContent", "[ctest][retail-dlc]")
{
    using rerevved::retail_dlc::CheckFileName;
    CHECK(CheckFileName("FAD23EDCA09BD08E933E68793F0E7EE43B8E9E6654").empty());
    CHECK_FALSE(CheckFileName(std::string(43, 'A')).empty());
    CHECK_FALSE(CheckFileName("").empty());
    CHECK_FALSE(CheckFileName("my pack").empty());
    CHECK_FALSE(CheckFileName("a:b").empty());
    CHECK_FALSE(CheckFileName("RR_rerevved-islands").empty());
}

TEST_CASE("retail DLC keeps the first package per token, slot and scenario ID", "[ctest][retail-dlc]")
{
    const std::vector<rerevved::retail_dlc::Candidate> candidates = {
        candidate("A", "victory", { 0 }, { 10, 11, 12 }),
        candidate("B", "Victory", { 0 }, { 10, 11, 12 }),
        candidate("C", "mpmp2", { 3 }, { 21, 22, 23, 24 }),
        candidate("D", "scenario2", { 3 }, { 21, 22, 23 }),
        candidate("E", "spmp2", { 4 }, { 24, 25, 26, 27 }),
        candidate("F", "mythic", {}, {}),
        candidate("G", "mythic", {}, {}),
        candidate("H", "brave", { 1 }, { 13, 12 }),
        candidate("I", "brave", { 1 }, { 13, 14, 15, 16 }),
        candidate("i", "iconic", {}, {}),
    };
    const auto decisions = rerevved::retail_dlc::SelectPackages(candidates);
    REQUIRE(decisions.size() == candidates.size());
    CHECK(decisions[0].accepted);
    CHECK(decisions[1].reason == "pack 'Victory' is already provided");
    CHECK(decisions[2].accepted);
    CHECK(decisions[3].reason == "scenario file slot 3 is already provided");
    CHECK(decisions[4].reason == "scenario ID 24 is already provided");
    CHECK(decisions[5].accepted);
    CHECK(decisions[6].reason == "pack 'mythic' is already provided");
    // A refused package claims nothing, so a later package with its token is accepted.
    CHECK(decisions[7].reason == "scenario ID 12 is already provided");
    CHECK(decisions[8].accepted);
    // Content names ignore case, so only one of I and i could register.
    CHECK(decisions[9].reason == "another file has the same name ignoring case");
}
