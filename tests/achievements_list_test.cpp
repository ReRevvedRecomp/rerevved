#include "achievements_list.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

namespace
{

namespace al = rerevved::achievements_list;
using rex::system::AchievementInfo;

AchievementInfo info(uint32_t id, const char* label, uint32_t gamerscore, bool custom, const char* locked = "hint", const char* unlocked = "done")
{
    AchievementInfo a;
    a.id                     = id;
    a.label                  = label;
    a.gamerscore             = gamerscore;
    a.custom                 = custom;
    a.description            = unlocked;
    a.unachieved_description = locked;
    return a;
}

const al::IsUnlockedFn kNone = [](uint32_t)
{
    return false;
};
const al::UnlockTimeFn kNoTime = [](uint32_t)
{
    return uint64_t{ 0 };
};

} // namespace

TEST_CASE("achievements_list_orders_original_by_catalog_then_custom_by_score", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos = {
        info(9, "Zeta", 50, true),
        info(3, "Third", 30, false),
        info(1, "First", 10, false),
        info(8, "Beta", 20, true),
        info(7, "Alpha", 20, true),
        info(6, "Alpha", 20, true),
        info(2, "Second", 90, false),
    };
    const auto            entries = al::Build(infos, kNone, kNoTime);
    std::vector<uint32_t> ids;
    for (const auto& entry : entries)
        ids.push_back(entry.id);
    // Catalog order for the originals; custom by gamerscore, label, then ID.
    CHECK(ids == std::vector<uint32_t>{ 3, 1, 2, 6, 7, 8, 9 });
}

TEST_CASE("achievements_list_picks_the_description_for_the_state", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos = {
        info(1, "A", 5, false, "locked text", "unlocked text"),
        info(2, "B", 5, false, "", "only text"),
    };
    const al::IsUnlockedFn unlocked = [](uint32_t id)
    {
        return id == 1;
    };
    const auto entries = al::Build(infos, unlocked, kNoTime);
    CHECK(entries[0].description == "unlocked text");
    CHECK(entries[1].description == "only text");
    const auto locked = al::Build(infos, kNone, kNoTime);
    CHECK(locked[0].description == "locked text");
    CHECK(locked[1].description == "only text");
}

TEST_CASE("achievements_list_keeps_unlock_time_only_when_unlocked", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos    = { info(1, "A", 5, false), info(2, "B", 5, false) };
    const al::IsUnlockedFn             unlocked = [](uint32_t id)
    {
        return id == 1;
    };
    const al::UnlockTimeFn time = [](uint32_t id)
    {
        return uint64_t{ id * 1000u };
    };
    const auto entries = al::Build(infos, unlocked, time);
    CHECK(entries[0].unlocked);
    CHECK(entries[0].unlockedAt == 1000);
    CHECK_FALSE(entries[1].unlocked);
    CHECK(entries[1].unlockedAt == 0);
}

TEST_CASE("achievements_list_sums_unlocked_count_and_gamerscore", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos    = { info(1, "A", 10, false), info(2, "B", 20, false), info(3, "C", 30, true) };
    const al::IsUnlockedFn             unlocked = [](uint32_t id)
    {
        return id != 2;
    };
    const auto totals = al::Summarize(al::Build(infos, unlocked, kNoTime));
    CHECK(totals.unlocked == 2);
    CHECK(totals.total == 3);
    CHECK(totals.earned == 40);
    CHECK(totals.possible == 60);
    const auto empty = al::Summarize({});
    CHECK(empty.total == 0);
    CHECK(empty.possible == 0);
}

TEST_CASE("achievements_list_substitutes_numbered_placeholders", "[ctest][achievements-list]")
{
    const std::vector<std::string> args = { "7", "58" };
    CHECK(al::Substitute("{0} of {1} unlocked", args) == "7 of 58 unlocked");
    CHECK(al::Substitute("{1}/{0}/{1}", args) == "58/7/58");
    CHECK(al::Substitute("{2} {x} { {0", args) == "{2} {x} { {0");
    CHECK(al::Substitute("no placeholders", args) == "no placeholders");
}

TEST_CASE("achievements_list_converts_file_times_to_dates", "[ctest][achievements-list]")
{
    CHECK_FALSE(al::UnixSeconds(0).has_value());
    CHECK_FALSE(al::UnixSeconds(116444735999999999ull).has_value());
    CHECK(al::UnixSeconds(116444736000000000ull) == 0);
    // 2001-09-09 01:46:40 UTC, 1,000,000,000 seconds after the epoch.
    CHECK(al::UnixSeconds(116444736000000000ull + 10000000000000000ull) == 1000000000);
    CHECK(al::FormatDate(2026, 3, 9) == "2026-03-09");
}

TEST_CASE("achievements_list_splits_into_the_f3_original_and_custom_tabs", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos = {
        info(9, "Zeta", 50, true),
        info(3, "Third", 30, false),
        info(1, "First", 10, false),
        info(8, "Beta", 20, true),
    };
    const auto entries = al::Build(infos, [](uint32_t id)
                                   {
                                       return id == 3 || id == 8;
                                   },
                                   kNoTime);
    const auto groups  = al::Split(entries);
    REQUIRE(groups.original.size() == 2);
    CHECK(groups.original[0].id == 3);
    CHECK(groups.original[1].id == 1);
    REQUIRE(groups.custom.size() == 2);
    CHECK(groups.custom[0].id == 8);
    CHECK(groups.custom[1].id == 9);
    CHECK(groups.HasTabs());
    CHECK(&groups.Tab(0) == &groups.original);
    CHECK(&groups.Tab(1) == &groups.custom);
    const auto original = al::Summarize(groups.original);
    CHECK(original.unlocked == 1);
    CHECK(original.total == 2);
    CHECK(original.earned == 30);
    CHECK(original.possible == 40);
    const auto custom = al::Summarize(groups.custom);
    CHECK(custom.unlocked == 1);
    CHECK(custom.total == 2);
    CHECK(custom.earned == 20);
    CHECK(custom.possible == 70);
}

TEST_CASE("achievements_list_has_no_tabs_without_custom_achievements", "[ctest][achievements-list]")
{
    const std::vector<AchievementInfo> infos  = { info(1, "First", 10, false), info(2, "Second", 20, false) };
    const auto                         groups = al::Split(al::Build(infos, kNone, kNoTime));
    CHECK_FALSE(groups.HasTabs());
    CHECK(groups.original.size() == 2);
    CHECK(groups.Tab(1).empty());
    CHECK(al::Split({}).original.empty());
}
