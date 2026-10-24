#include "gameplay_fixes.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <vector>

namespace gf = rerevved::gameplay_fixes;

namespace
{

// The eligible list as sub_82CECCD8 builds it: ids ascending, then -1.
std::vector<int32_t> makeList(int32_t eligible)
{
    std::vector<int32_t> list;
    for (int32_t i = 0; i < eligible; ++i)
    {
        list.push_back(i + 1);
    }
    list.push_back(-1);
    return list;
}

} // namespace

TEST_CASE("ai_research_scan_visits_each_eligible_id_once", "[ctest][gameplay-fixes]")
{
    for (int32_t eligible = 0; eligible <= 47; ++eligible)
    {
        const auto           list     = makeList(eligible);
        const int32_t        retained = static_cast<int32_t>(list.size());
        std::vector<int32_t> visited;
        const uint32_t       start = gf::AiResearchScanStart(0) / 4;
        for (uint32_t i = 0; i < gf::AiResearchScanCount(retained); ++i)
        {
            visited.push_back(list.at(start + i));
        }
        UNSCOPED_INFO("eligible=" << eligible);
        CHECK(visited.size() == static_cast<size_t>(eligible));
        CHECK(std::set<int32_t>(visited.begin(), visited.end()).size() == visited.size());
        CHECK(std::set<int32_t>(visited.begin(), visited.end()) == std::set<int32_t>(list.begin(), list.end() - 1));
        for (const int32_t id : visited)
        {
            CHECK(id != -1);
        }
    }
}

TEST_CASE("ai_research_scan_keeps_native_count_and_empty_branch", "[ctest][gameplay-fixes]")
{
    CHECK(gf::AiResearchScanCount(1) == 0);
    CHECK(gf::AiResearchScanCount(0) == 0);
    CHECK(gf::AiResearchScanCount(2) == 1);
    CHECK(gf::AiResearchScanCount(48) == 47);
    CHECK(gf::AiResearchScanStart(0x1000) == 0x1000);
}

TEST_CASE("spaceship_arrival_year_admission", "[ctest][gameplay-fixes]")
{
    CHECK(gf::AdmitsSpaceshipArrivalYear(2050));
    CHECK(gf::AdmitsSpaceshipArrivalYear(1));
    CHECK(gf::AdmitsSpaceshipArrivalYear(0));
    CHECK(gf::AdmitsSpaceshipArrivalYear(-100));
    CHECK(gf::AdmitsSpaceshipArrivalYear(-3200));
    CHECK(!gf::AdmitsSpaceshipArrivalYear(-1));
}

TEST_CASE("domination_minimum_turns_parsing", "[ctest][gameplay-fixes]")
{
    int32_t turns = 0;
    CHECK((gf::ParseDominationMinimumTurns("10", turns) && turns == 10));
    CHECK((gf::ParseDominationMinimumTurns("5", turns) && turns == 5));
    CHECK((gf::ParseDominationMinimumTurns("1", turns) && turns == 1));
    turns = 7;
    for (const char* bad : { "", "0", "2", "11", "-1", " 5", "5 ", "05", "x" })
    {
        UNSCOPED_INFO("text=" << bad);
        CHECK(!gf::ParseDominationMinimumTurns(bad, turns));
        CHECK(turns == 7);
    }
}

TEST_CASE("domination_minimum_turns_policy", "[ctest][gameplay-fixes]")
{
    CHECK(gf::DominationMinimumTurns("1", true) == 1);
    CHECK(gf::DominationMinimumTurns("5", true) == 5);
    CHECK(gf::DominationMinimumTurns("10", true) == 10);
    CHECK(gf::DominationMinimumTurns("1", false) == 10);
    CHECK(gf::DominationMinimumTurns("bogus", true) == 10);
}

TEST_CASE("domination_shortened_minimum_latch", "[ctest][gameplay-fixes]")
{
    CHECK(!gf::ShortenedMinimumAllowed());
    gf::NoteNewGame(false);
    CHECK(gf::ShortenedMinimumAllowed());
    gf::NoteNewGame(true);
    CHECK(!gf::ShortenedMinimumAllowed());
    gf::NoteNewGame(false);
    gf::NoteGameLoad();
    CHECK(!gf::ShortenedMinimumAllowed());
}

TEST_CASE("domination_elapsed_rebase_matches_threshold", "[ctest][gameplay-fixes]")
{
    for (const int32_t minimum : { 1, 5, 10 })
    {
        for (int32_t elapsed = -3; elapsed <= 40; ++elapsed)
        {
            UNSCOPED_INFO("minimum=" << minimum << " elapsed=" << elapsed);
            CHECK((gf::RebaseDominationElapsed(elapsed, minimum) >= 10) == (elapsed >= minimum));
        }
    }
    for (int32_t elapsed = -3; elapsed <= 40; ++elapsed)
    {
        CHECK(gf::RebaseDominationElapsed(elapsed, 10) == elapsed);
    }
    CHECK(gf::RebaseDominationElapsed(INT32_MAX, 1) == INT32_MAX);
    CHECK(gf::RebaseDominationElapsed(INT32_MIN, 10) == INT32_MIN);
}
