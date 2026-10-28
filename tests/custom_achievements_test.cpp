#include "ai_economy_settings.h"
#include "custom_achievements.h"
#include "cvar_scope.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include <rex/system/achievement_manager.h>

REXCVAR_DECLARE(int32_t, ai_research_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_production_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_gold_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_food_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_culture_bonus_percent);

namespace
{

namespace achievements = rerevved::custom_achievements;
namespace economy      = rerevved::ai_economy;

constexpr economy::Policy kMaxPolicy{ 200, 200, 200, 200, 200 };

achievements::VictoryContext qualifying(int32_t type = 1)
{
    return { type, 4, false, false, kMaxPolicy };
}

struct FakeStore
{
    std::set<uint32_t> unlocked;
    int                unlockCalls = 0;
    bool               succeed     = true;

    achievements::AwardResult award(const achievements::VictoryContext& context)
    {
        return achievements::Award(
            context,
            [this](uint32_t id)
            {
                return unlocked.contains(id);
            },
            [this](uint32_t id)
            {
                ++unlockCalls;
                if (!succeed)
                {
                    return false;
                }
                return unlocked.insert(id).second;
            });
    }

    achievements::AwardResult awardOneToRule(const achievements::VictoryContext& context)
    {
        return achievements::AwardOneToRule(
            context,
            [this](uint32_t id)
            {
                return unlocked.contains(id);
            },
            [this](uint32_t id)
            {
                ++unlockCalls;
                return unlocked.insert(id).second;
            });
    }

    achievements::AwardResult recordDefeat()
    {
        return achievements::RecordBarbarianDefeat(
            [this](uint32_t id)
            {
                return unlocked.contains(id);
            },
            [this](uint32_t id)
            {
                ++unlockCalls;
                return unlocked.insert(id).second;
            });
    }
};

achievements::VictoryContext qualifyingOneToRule(int32_t type = 1)
{
    auto context         = qualifying(type);
    context.occEnabled   = true;
    context.occFromSetup = true;
    return context;
}

// The local player 0 defeats a unit of Barbarian player 5 at a combat site.
achievements::DefeatEvent localKillsBarbarian()
{
    return { true, true, 5, 0, 0, 5, false, true };
}

// A tracked Deity win in 2000 AD with no handicaps, no battles and no units: it
// satisfies only the history achievements, and each test varies one fact.
achievements::VictoryContext tracked(int32_t type = 1)
{
    achievements::VictoryContext context;
    context.victoryType        = type;
    context.difficulty         = 4;
    context.yearKnown          = true;
    context.year               = 2000;
    context.historyKnown       = true;
    context.firstFoundingTurn  = 0;
    context.initialAiOpponents = 4;
    return context;
}

achievements::VictoryContext withHandicaps(achievements::VictoryContext context, int32_t percent)
{
    context.policy = { percent, percent, percent, percent, percent };
    return context;
}

// The achievements a fresh profile unlocks for this victory.
std::set<uint32_t> unlockedBy(const achievements::VictoryContext& context)
{
    std::set<uint32_t> unlocked;
    for (const auto& decision : achievements::AwardVictory(
             context,
             [&unlocked](uint32_t id)
             {
                 return unlocked.contains(id);
             },
             [&unlocked](uint32_t id)
             {
                 return unlocked.insert(id).second;
             }))
    {
        CHECK((decision.result == achievements::AwardResult::Unlocked) == decision.qualifies);
    }
    return unlocked;
}

// Runs one event record call against a fresh unlock store.
achievements::AwardResult eventAward(auto record)
{
    std::set<uint32_t> unlocked;
    return record(
        [&unlocked](uint32_t id)
        {
            return unlocked.contains(id);
        },
        [&unlocked](uint32_t id)
        {
            return unlocked.insert(id).second;
        });
}

std::array<uint8_t, 1600> openSea()
{
    return {};
}

void setTile(std::array<uint8_t, 1600>& terrain, int32_t x, int32_t y, uint8_t value)
{
    terrain[static_cast<size_t>(40 * x + y)] = value;
}

struct ProgressReset
{
    ~ProgressReset()
    {
        achievements::ResetProgress();
    }
};

struct PolicyReset
{
    ~PolicyReset()
    {
        economy::RestoreForGame({});
        economy::ResetForLoad();
        economy::FinishLoad(false);
    }
};

} // namespace

TEST_CASE("custom_achievement_metadata", "[ctest][custom-achievements]")
{
    const auto info = achievements::ConcentratedPowerOfWill();
    CHECK(info.id == 0x10001);
    CHECK(info.id == achievements::kConcentratedPowerOfWillId);
    CHECK(info.id > 0xFFFF);
    CHECK(info.label == "200% Concentrated Power of Will");
    CHECK(info.description == "Win a game on Deity with all five AI Handicaps set to 200% for the entire game.");
    CHECK(info.unachieved_description == info.description);
    CHECK(info.gamerscore == 200);
    CHECK(info.icon_path == "icons/200-percent-concentrated-power-of-will.png");
    CHECK(info.image_id == 0);
    CHECK(info.flags == 0);
}

TEST_CASE("custom_achievement_qualifies_all_victory_types", "[ctest][custom-achievements]")
{
    for (int32_t type = 1; type <= 4; ++type)
    {
        CAPTURE(type);
        CHECK(achievements::Qualifies(qualifying(type)));
    }
}

TEST_CASE("custom_achievement_rejects_non_qualifying_games", "[ctest][custom-achievements]")
{
    for (int32_t value : { 199, 0 })
    {
        CAPTURE(value);
        auto context                        = qualifying();
        context.policy.researchBonusPercent = value;
        CHECK_FALSE(achievements::Qualifies(context));
        context                               = qualifying();
        context.policy.productionBonusPercent = value;
        CHECK_FALSE(achievements::Qualifies(context));
        context                         = qualifying();
        context.policy.goldBonusPercent = value;
        CHECK_FALSE(achievements::Qualifies(context));
        context                         = qualifying();
        context.policy.foodBonusPercent = value;
        CHECK_FALSE(achievements::Qualifies(context));
        context                            = qualifying();
        context.policy.cultureBonusPercent = value;
        CHECK_FALSE(achievements::Qualifies(context));
    }

    for (int32_t difficulty : { 3, 0, -1 })
    {
        CAPTURE(difficulty);
        auto context       = qualifying();
        context.difficulty = difficulty;
        CHECK_FALSE(achievements::Qualifies(context));
    }

    auto context        = qualifying();
    context.networkGame = true;
    CHECK_FALSE(achievements::Qualifies(context));
    context               = qualifying();
    context.nativeEconomy = true;
    CHECK_FALSE(achievements::Qualifies(context));
}

TEST_CASE("custom_achievement_award_once", "[ctest][custom-achievements]")
{
    FakeStore store;
    CHECK(store.award(qualifying()) == achievements::AwardResult::Unlocked);
    CHECK(store.award(qualifying(4)) == achievements::AwardResult::AlreadyUnlocked);
    CHECK(store.unlockCalls == 1);
}

TEST_CASE("custom_achievement_award_skips_non_qualifying", "[ctest][custom-achievements]")
{
    FakeStore store;
    auto      context                  = qualifying();
    context.policy.cultureBonusPercent = 199;
    CHECK(store.award(context) == achievements::AwardResult::NotQualified);
    CHECK(store.unlockCalls == 0);
    CHECK(store.unlocked.empty());
}

TEST_CASE("custom_achievement_award_reports_failed_unlock", "[ctest][custom-achievements]")
{
    FakeStore store;
    store.succeed = false;
    CHECK(store.award(qualifying()) == achievements::AwardResult::Failed);
    CHECK(store.unlockCalls == 1);
}

TEST_CASE("custom_achievement_duplicate_delivery_notifies_once", "[ctest][custom-achievements]")
{
    rex::system::AchievementManager manager;
    REQUIRE(manager.RegisterAchievement(achievements::ConcentratedPowerOfWill()));
    int notifications = 0;
    manager.RegisterNotificationCallback(
        [&notifications](const rex::system::AchievementEvent&)
        {
            ++notifications;
        });

    const auto award = [&manager]
    {
        return achievements::Award(
            qualifying(),
            [&manager](uint32_t id)
            {
                return manager.IsUnlocked(id);
            },
            [&manager](uint32_t id)
            {
                return manager.UnlockAchievement(id, rex::system::AchievementNotification::kShow) ==
                       rex::system::AchievementUnlockResult::kUnlocked;
            });
    };
    CHECK(award() == achievements::AwardResult::Unlocked);
    CHECK(award() == achievements::AwardResult::AlreadyUnlocked);
    CHECK(notifications == 1);
    CHECK(manager.IsUnlocked(achievements::kConcentratedPowerOfWillId));
}

TEST_CASE("custom_achievement_frozen_policy_decision", "[ctest][custom-achievements]")
{
    const PolicyReset reset;
    REREVVED_RESTORE_CVAR(ai_research_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_production_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_gold_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_food_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_culture_bonus_percent);

    const auto decide = []
    {
        auto context   = qualifying();
        context.policy = economy::ForGame();
        return achievements::Qualifies(context);
    };
    const auto setSliders = [](int32_t value)
    {
        REXCVAR_SET(ai_research_bonus_percent, value);
        REXCVAR_SET(ai_production_bonus_percent, value);
        REXCVAR_SET(ai_gold_bonus_percent, value);
        REXCVAR_SET(ai_food_bonus_percent, value);
        REXCVAR_SET(ai_culture_bonus_percent, value);
    };

    SECTION("slider edits after freezing do not change the decision")
    {
        setSliders(200);
        economy::FreezeForGame();
        CHECK(decide());
        setSliders(0);
        CHECK(decide());

        setSliders(0);
        economy::FreezeForGame();
        CHECK_FALSE(decide());
        setSliders(200);
        CHECK_FALSE(decide());
    }

    SECTION("a load without a sidecar restore disqualifies")
    {
        economy::RestoreForGame(kMaxPolicy);
        CHECK(decide());
        economy::BeginLoad();
        economy::ResetForLoad();
        economy::FinishLoad(false);
        CHECK_FALSE(decide());
    }

    SECTION("a load that restores the policy after reset qualifies")
    {
        economy::BeginLoad();
        economy::ResetForLoad();
        economy::RestoreForGame(kMaxPolicy);
        economy::FinishLoad(false);
        CHECK(decide());
    }

    SECTION("Standard Rules disqualify")
    {
        setSliders(200);
        economy::FreezeForGame(true);
        CHECK_FALSE(decide());
    }
}

TEST_CASE("custom_achievement_new_metadata", "[ctest][custom-achievements]")
{
    const auto rule = achievements::OneToRule();
    CHECK(rule.id == 0x10002);
    CHECK(rule.id == achievements::kOneToRuleId);
    CHECK(rule.label == "One To Rule Them All");
    CHECK(rule.description == "Win a game on Deity with One City Challenge enabled.");
    CHECK(rule.unachieved_description == rule.description);
    CHECK(rule.gamerscore == 15);
    CHECK(rule.image_id == 5);
    CHECK(rule.icon_path.empty());
    CHECK(rule.flags == 0);

    const auto rage = achievements::TheirRageIsWithinYourControl();
    CHECK(rage.id == 0x10003);
    CHECK(rage.id == achievements::kTheirRageId);
    CHECK(rage.label == "Their Rage Is Within Your Control");
    CHECK(rage.description == "Defeat 50 Barbarian units in a single game.");
    CHECK(rage.unachieved_description == rage.description);
    CHECK(rage.gamerscore == 25);
    CHECK(rage.image_id == 21);
    CHECK(rage.icon_path.empty());
    CHECK(rage.flags == 0);

    const std::set<uint32_t> ids{ achievements::kConcentratedPowerOfWillId, rule.id, rage.id };
    CHECK(ids.size() == 3);
    CHECK(*ids.begin() > 0xFFFF);
}

TEST_CASE("custom_achievement_one_to_rule_qualifies", "[ctest][custom-achievements]")
{
    for (int32_t type = 1; type <= 4; ++type)
    {
        CAPTURE(type);
        CHECK(achievements::QualifiesOneToRule(qualifyingOneToRule(type)));
    }

    auto context       = qualifyingOneToRule();
    context.occEnabled = false;
    CHECK_FALSE(achievements::QualifiesOneToRule(context));
    // OCC switched on mid-game by a load without a sidecar.
    context              = qualifyingOneToRule();
    context.occFromSetup = false;
    CHECK_FALSE(achievements::QualifiesOneToRule(context));
    for (int32_t difficulty : { 3, 0, -1 })
    {
        CAPTURE(difficulty);
        context            = qualifyingOneToRule();
        context.difficulty = difficulty;
        CHECK_FALSE(achievements::QualifiesOneToRule(context));
    }
    context             = qualifyingOneToRule();
    context.networkGame = true;
    CHECK_FALSE(achievements::QualifiesOneToRule(context));
}

TEST_CASE("custom_achievement_one_to_rule_is_independent", "[ctest][custom-achievements]")
{
    // One win may satisfy both.
    FakeStore  store;
    const auto both = qualifyingOneToRule();
    CHECK(achievements::Qualifies(both));
    CHECK(achievements::QualifiesOneToRule(both));
    CHECK(store.award(both) == achievements::AwardResult::Unlocked);
    CHECK(store.awardOneToRule(both) == achievements::AwardResult::Unlocked);
    CHECK(store.unlocked == std::set<uint32_t>{ achievements::kConcentratedPowerOfWillId, achievements::kOneToRuleId });
    CHECK(store.awardOneToRule(both) == achievements::AwardResult::AlreadyUnlocked);

    // Either predicate holds without the other.
    auto occOnly                    = qualifyingOneToRule();
    occOnly.policy.goldBonusPercent = 0;
    CHECK_FALSE(achievements::Qualifies(occOnly));
    CHECK(achievements::QualifiesOneToRule(occOnly));
    CHECK(achievements::Qualifies(qualifying()));
    CHECK_FALSE(achievements::QualifiesOneToRule(qualifying()));

    FakeStore other;
    CHECK(other.awardOneToRule(qualifying()) == achievements::AwardResult::NotQualified);
    CHECK(other.unlockCalls == 0);
}

TEST_CASE("custom_achievement_progress_flow", "[ctest][custom-achievements]")
{
    const ProgressReset reset;

    achievements::BeginNewGame(true, false);
    CHECK(achievements::CurrentProgress() == achievements::Progress{ true, 0, true });

    // A load without a sidecar must not keep the previous game setup flag.
    achievements::ResetProgress();
    CHECK(achievements::CurrentProgress() == achievements::Progress{});

    achievements::RestoreProgress({ true, 30 });
    CHECK(achievements::CurrentProgress() == achievements::Progress{ true, 30 });

    achievements::BeginNewGame(false, false);
    CHECK(achievements::CurrentProgress() == achievements::Progress{ false, 0, true });

    achievements::RestoreProgress({ true, 51 });
    CHECK(achievements::CurrentProgress() == achievements::Progress{ true, 50 });
}

TEST_CASE("custom_achievement_counts_local_barbarian_defeats", "[ctest][custom-achievements]")
{
    // The local player may occupy any slot if the winner and local words agree.
    CHECK(achievements::CountsAsBarbarianDefeat(localKillsBarbarian()));
    auto other   = localKillsBarbarian();
    other.winner = 2;
    other.local  = 2;
    CHECK(achievements::CountsAsBarbarianDefeat(other));

    const auto rejects = [](auto change)
    {
        auto event = localKillsBarbarian();
        change(event);
        return !achievements::CountsAsBarbarianDefeat(event);
    };
    CHECK(rejects(
        [](auto& e)
        {
            e.loser = 1;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.winner = 1;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.local  = -1;
            e.winner = -1;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.barbarian = -1;
            e.loser     = -1;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.combatSite = false;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.participantsMatch = false;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.killedBefore = true;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.killedAfter = false;
        }));
    CHECK(rejects(
        [](auto& e)
        {
            e.winner = 5;
            e.local  = 5;
        }));
}

TEST_CASE("custom_achievement_barbarian_defeats_unlock_once", "[ctest][custom-achievements]")
{
    const ProgressReset reset;
    achievements::BeginNewGame(false, false);

    FakeStore store;
    for (int i = 1; i < 50; ++i)
    {
        CAPTURE(i);
        CHECK(store.recordDefeat() == achievements::AwardResult::NotQualified);
    }
    CHECK(achievements::CurrentProgress().barbarianDefeats == 49);
    CHECK(store.unlockCalls == 0);

    CHECK(store.recordDefeat() == achievements::AwardResult::Unlocked);
    CHECK(store.unlocked.contains(achievements::kTheirRageId));
    CHECK(achievements::CurrentProgress().barbarianDefeats == 50);

    CHECK(store.recordDefeat() == achievements::AwardResult::AlreadyUnlocked);
    CHECK(store.recordDefeat() == achievements::AwardResult::AlreadyUnlocked);
    CHECK(achievements::CurrentProgress().barbarianDefeats == 50);
    CHECK(store.unlockCalls == 1);
}

TEST_CASE("custom_achievement_barbarian_defeats_retry_saturated_unlock", "[ctest][custom-achievements]")
{
    const ProgressReset reset;
    // A save with 50 defeats loaded under a profile without the unlock.
    achievements::RestoreProgress({ false, 50 });

    FakeStore store;
    CHECK(store.recordDefeat() == achievements::AwardResult::Unlocked);
    CHECK(achievements::CurrentProgress().barbarianDefeats == 50);
    CHECK(store.unlockCalls == 1);
}

TEST_CASE("custom_achievement_barbarian_defeats_with_existing_unlock", "[ctest][custom-achievements]")
{
    const ProgressReset reset;
    achievements::BeginNewGame(false, false);

    FakeStore store;
    store.unlocked.insert(achievements::kTheirRageId);
    for (int i = 1; i < 50; ++i)
    {
        CHECK(store.recordDefeat() == achievements::AwardResult::NotQualified);
    }
    CHECK(store.recordDefeat() == achievements::AwardResult::AlreadyUnlocked);
    CHECK(store.recordDefeat() == achievements::AwardResult::AlreadyUnlocked);
    CHECK(achievements::CurrentProgress().barbarianDefeats == 50);
    CHECK(store.unlockCalls == 0);
}

TEST_CASE("custom_achievement_barbarian_defeats_continue_after_reload", "[ctest][custom-achievements]")
{
    const ProgressReset reset;
    achievements::BeginNewGame(false, false);
    FakeStore store;
    for (int i = 0; i < 10; ++i)
    {
        store.recordDefeat();
    }

    // A reload restores the saved count, not the live one.
    achievements::ResetProgress();
    achievements::RestoreProgress({ false, 30 });
    for (int i = 0; i < 19; ++i)
    {
        CHECK(store.recordDefeat() == achievements::AwardResult::NotQualified);
    }
    CHECK(achievements::CurrentProgress().barbarianDefeats == 49);
    CHECK(store.recordDefeat() == achievements::AwardResult::Unlocked);
}

TEST_CASE("custom_achievement_metadata_of_all_seventeen", "[ctest][custom-achievements]")
{
    struct Row
    {
        uint32_t    id;
        const char* label;
        uint32_t    score;
        uint32_t    image;
    };

    constexpr Row rows[] = {
        { 0x10001, "200% Concentrated Power of Will", 200, 0 },
        { 0x10002, "One To Rule Them All", 15, 5 },
        { 0x10003, "Their Rage Is Within Your Control", 25, 21 },
        { 0x10004, "100% Reason to Remember the Name", 125, 0 },
        { 0x10005, "Halcyon Templar", 5, 19 },
        { 0x10006, "Beyond the Firmament", 5, 4 },
        { 0x10007, "Blockbuster World Champion", 50, 34 },
        { 0x10008, "Shallow Worldview", 20, 9 },
        { 0x10009, "Cosmic Eclipse", 75, 32 },
        { 0x1000A, "Art of the Walk In", 20, 12 },
        { 0x1000B, "Last Stand", 50, 1 },
        { 0x1000C, "Weekend Warrior", 100, 13 },
        { 0x1000D, "Ye Are Idle, Ye Are Idle", 75, 20 },
        { 0x1000E, "Leap Through Time", 10, 22 },
        { 0x1000F, "I Have No Enemies", 50, 51 },
        { 0x10010, "No More Worlds To Conquer", 30, 37 },
        { 0x10011, "Tactically Centered", 100, 21 },
    };
    const auto all = achievements::AllAchievements();
    REQUIRE(all.size() == std::size(rows));
    uint32_t total = 0;
    for (size_t i = 0; i < all.size(); ++i)
    {
        CAPTURE(i);
        CHECK(all[i].id == rows[i].id);
        CHECK(all[i].label == rows[i].label);
        CHECK(all[i].gamerscore == rows[i].score);
        CHECK(all[i].image_id == rows[i].image);
        CHECK(all[i].unachieved_description == all[i].description);
        CHECK_FALSE(all[i].description.empty());
        CHECK(all[i].flags == 0);
        CHECK((i == 0 || i == 3) == !all[i].icon_path.empty());
        CHECK(std::string_view(achievements::AchievementName(rows[i].id)) == rows[i].label);
        total += all[i].gamerscore;
    }
    CHECK(total == 955);
    CHECK(all[3].icon_path == "icons/100-percent-reason-to-remember-the-name.png");
    CHECK(std::string_view(achievements::AchievementName(0x1FFFF)) == "unknown");
    CHECK(achievements::ConcentratedPowerOfWill().label == all[0].label);
    CHECK(achievements::OneToRule().label == all[1].label);
    CHECK(achievements::TheirRageIsWithinYourControl().label == all[2].label);
    CHECK(all[8].description == "Win a technology victory on or before 100 BC.");
    CHECK(all[13].description == "Win a Game of the Week on Week 36428.");
}

TEST_CASE("custom_achievement_handicap_tiers", "[ctest][custom-achievements]")
{
    const auto context = tracked();
    // Each tier needs all five handicaps exactly at its percentage.
    for (const int32_t percent : { 200, 100, 50 })
    {
        CAPTURE(percent);
        const auto unlocked = unlockedBy(withHandicaps(context, percent));
        CHECK(unlocked.contains(achievements::kConcentratedPowerOfWillId) == (percent == 200));
        CHECK(unlocked.contains(achievements::kReasonToRememberId) == (percent == 100));
        CHECK(unlocked.contains(achievements::kTacticallyCenteredId) == (percent == 50));
    }
    CHECK_FALSE(achievements::QualifiesReasonToRemember(withHandicaps(context, 0)));
    CHECK_FALSE(achievements::QualifiesTacticallyCentered(withHandicaps(context, 0)));

    for (const int32_t percent : { 100, 50 })
    {
        for (const int32_t off : { percent - 1, percent + 1 })
        {
            CAPTURE(percent, off);
            for (int slider = 0; slider < 5; ++slider)
            {
                auto     changed   = withHandicaps(context, percent);
                int32_t* values[5] = { &changed.policy.researchBonusPercent, &changed.policy.productionBonusPercent, &changed.policy.goldBonusPercent, &changed.policy.foodBonusPercent, &changed.policy.cultureBonusPercent };
                *values[slider]    = off;
                CHECK_FALSE(achievements::QualifiesReasonToRemember(changed));
                CHECK_FALSE(achievements::QualifiesTacticallyCentered(changed));
            }
        }
    }

    for (const int32_t percent : { 100, 50 })
    {
        CAPTURE(percent);
        const auto base   = withHandicaps(context, percent);
        const auto passes = [percent](const achievements::VictoryContext& c)
        {
            return percent == 100 ? achievements::QualifiesReasonToRemember(c) : achievements::QualifiesTacticallyCentered(c);
        };
        REQUIRE(passes(base));
        auto changed       = base;
        changed.difficulty = 3;
        CHECK_FALSE(passes(changed));
        changed             = base;
        changed.networkGame = true;
        CHECK_FALSE(passes(changed));
        changed               = base;
        changed.nativeEconomy = true;
        CHECK_FALSE(passes(changed));
        // The victory type does not matter.
        for (int32_t type = 1; type <= 4; ++type)
        {
            changed             = base;
            changed.victoryType = type;
            CHECK(passes(changed));
        }
    }

    // Tactically Centered also needs tracked history with no unit built. The 100% tier does not.
    auto fifty      = withHandicaps(context, 50);
    fifty.unitBuilt = true;
    CHECK_FALSE(achievements::QualifiesTacticallyCentered(fifty));
    fifty.unitBuilt    = false;
    fifty.historyKnown = false;
    CHECK_FALSE(achievements::QualifiesTacticallyCentered(fifty));
    auto hundred         = withHandicaps(context, 100);
    hundred.unitBuilt    = true;
    hundred.historyKnown = false;
    CHECK(achievements::QualifiesReasonToRemember(hundred));
}

TEST_CASE("custom_achievement_blockbuster_boundaries", "[ctest][custom-achievements]")
{
    auto context = tracked();
    for (const auto [year, expected] : { std::pair{ 1993, true }, std::pair{ 1994, true }, std::pair{ 1995, false }, std::pair{ -4000, true } })
    {
        CAPTURE(year);
        context.year = year;
        CHECK(achievements::QualifiesBlockbusterChampion(context) == expected);
    }
    context           = tracked();
    context.year      = 1994;
    context.yearKnown = false;
    CHECK_FALSE(achievements::QualifiesBlockbusterChampion(context));
    context            = tracked();
    context.year       = 1994;
    context.difficulty = 3;
    CHECK_FALSE(achievements::QualifiesBlockbusterChampion(context));
    context           = tracked();
    context.year      = 1994;
    context.unitBuilt = true;
    CHECK_FALSE(achievements::QualifiesBlockbusterChampion(context));
    context              = tracked();
    context.year         = 1994;
    context.historyKnown = false;
    CHECK_FALSE(achievements::QualifiesBlockbusterChampion(context));
    context             = tracked();
    context.year        = 1994;
    context.networkGame = true;
    CHECK_FALSE(achievements::QualifiesBlockbusterChampion(context));
}

TEST_CASE("custom_achievement_shallow_worldview_and_no_enemies", "[ctest][custom-achievements]")
{
    auto context = tracked();
    CHECK(achievements::QualifiesShallowWorldview(context));
    CHECK(achievements::QualifiesNoEnemies(context));

    // Shallow Worldview applies to any difficulty. I Have No Enemies needs Deity.
    context.difficulty = 0;
    CHECK(achievements::QualifiesShallowWorldview(context));
    CHECK_FALSE(achievements::QualifiesNoEnemies(context));
    context.difficulty = 3;
    CHECK_FALSE(achievements::QualifiesNoEnemies(context));

    context                  = tracked();
    context.deepOceanEntered = true;
    CHECK_FALSE(achievements::QualifiesShallowWorldview(context));
    CHECK(achievements::QualifiesNoEnemies(context));
    context              = tracked();
    context.battleFought = true;
    CHECK(achievements::QualifiesShallowWorldview(context));
    CHECK_FALSE(achievements::QualifiesNoEnemies(context));

    context             = tracked();
    context.networkGame = true;
    CHECK_FALSE(achievements::QualifiesShallowWorldview(context));
    CHECK_FALSE(achievements::QualifiesNoEnemies(context));
}

TEST_CASE("custom_achievement_cosmic_eclipse_boundaries", "[ctest][custom-achievements]")
{
    auto context = tracked(2);
    for (const auto [year, expected] : { std::pair{ -200, true }, std::pair{ -100, true }, std::pair{ -99, false }, std::pair{ 1000, false } })
    {
        CAPTURE(year);
        context.year = year;
        CHECK(achievements::QualifiesCosmicEclipse(context) == expected);
    }
    // Any difficulty and no history requirement, but only a technology victory.
    context              = tracked(2);
    context.year         = -100;
    context.difficulty   = 0;
    context.historyKnown = false;
    CHECK(achievements::QualifiesCosmicEclipse(context));
    for (const int32_t type : { 1, 3, 4 })
    {
        context.victoryType = type;
        CHECK_FALSE(achievements::QualifiesCosmicEclipse(context));
    }
    context           = tracked(2);
    context.year      = -100;
    context.yearKnown = false;
    CHECK_FALSE(achievements::QualifiesCosmicEclipse(context));
    context             = tracked(2);
    context.year        = -100;
    context.networkGame = true;
    CHECK_FALSE(achievements::QualifiesCosmicEclipse(context));
}

TEST_CASE("custom_achievement_game_of_the_week_predicates", "[ctest][custom-achievements]")
{
    auto weekend     = tracked();
    weekend.gotwGame = true;
    for (const auto [year, expected] : { std::pair{ -3100, true }, std::pair{ -3001, true }, std::pair{ -3000, false }, std::pair{ -2999, false } })
    {
        CAPTURE(year);
        weekend.year = year;
        CHECK(achievements::QualifiesWeekendWarrior(weekend) == expected);
    }
    weekend.year     = -3001;
    auto changed     = weekend;
    changed.gotwGame = false;
    CHECK_FALSE(achievements::QualifiesWeekendWarrior(changed));
    changed            = weekend;
    changed.difficulty = 3;
    CHECK_FALSE(achievements::QualifiesWeekendWarrior(changed));
    changed           = weekend;
    changed.yearKnown = false;
    CHECK_FALSE(achievements::QualifiesWeekendWarrior(changed));
    changed             = weekend;
    changed.networkGame = true;
    CHECK_FALSE(achievements::QualifiesWeekendWarrior(changed));

    auto leap       = tracked();
    leap.gotwGame   = true;
    leap.gotwWeek   = 36428;
    leap.difficulty = 0;
    CHECK(achievements::QualifiesLeapThroughTime(leap));
    leap.gotwWeek = 36427;
    CHECK_FALSE(achievements::QualifiesLeapThroughTime(leap));
    leap.gotwWeek = 36429;
    CHECK_FALSE(achievements::QualifiesLeapThroughTime(leap));
    leap.gotwWeek = 36428;
    leap.gotwGame = false;
    CHECK_FALSE(achievements::QualifiesLeapThroughTime(leap));
    leap.gotwGame    = true;
    leap.networkGame = true;
    CHECK_FALSE(achievements::QualifiesLeapThroughTime(leap));
    leap          = tracked();
    leap.gotwGame = true;
    leap.gotwWeek = 0;
    CHECK_FALSE(achievements::QualifiesLeapThroughTime(leap));
}

TEST_CASE("custom_achievement_late_founding_boundaries", "[ctest][custom-achievements]")
{
    auto context = tracked();
    for (const auto [turn, expected] : { std::pair{ 99, false }, std::pair{ 100, true }, std::pair{ 101, true }, std::pair{ 0, false }, std::pair{ -1, false } })
    {
        CAPTURE(turn);
        context.firstFoundingTurn = turn;
        CHECK(achievements::QualifiesYeAreIdle(context) == expected);
    }
    context.firstFoundingTurn = 100;
    auto changed              = context;
    changed.historyKnown      = false;
    CHECK_FALSE(achievements::QualifiesYeAreIdle(changed));
    changed            = context;
    changed.difficulty = 3;
    CHECK_FALSE(achievements::QualifiesYeAreIdle(changed));
    changed             = context;
    changed.networkGame = true;
    CHECK_FALSE(achievements::QualifiesYeAreIdle(changed));
}

TEST_CASE("custom_achievement_domination_with_six_opponents", "[ctest][custom-achievements]")
{
    auto context = tracked(1);
    for (const auto [opponents, expected] : { std::pair{ 6, true }, std::pair{ 5, false }, std::pair{ 7, false }, std::pair{ 4, false }, std::pair{ -1, false } })
    {
        CAPTURE(opponents);
        context.initialAiOpponents = opponents;
        CHECK(achievements::QualifiesNoMoreWorlds(context) == expected);
    }
    context.initialAiOpponents = 6;
    for (const int32_t type : { 2, 3, 4 })
    {
        context.victoryType = type;
        CHECK_FALSE(achievements::QualifiesNoMoreWorlds(context));
    }
    context                    = tracked(1);
    context.initialAiOpponents = 6;
    context.difficulty         = 3;
    CHECK_FALSE(achievements::QualifiesNoMoreWorlds(context));
    context                    = tracked(1);
    context.initialAiOpponents = 6;
    context.networkGame        = true;
    CHECK_FALSE(achievements::QualifiesNoMoreWorlds(context));
}

TEST_CASE("custom_achievement_untracked_history_fails_whole_game_achievements", "[ctest][custom-achievements]")
{
    // A load without a schema 2 sidecar: every history-based achievement fails even when
    // the remembered values would qualify.
    auto context              = withHandicaps(tracked(1), 50);
    context.year              = 1000;
    context.firstFoundingTurn = 150;
    CHECK(unlockedBy(context).contains(achievements::kTacticallyCenteredId));
    context.historyKnown = false;
    const auto unlocked  = unlockedBy(context);
    for (const uint32_t id : { achievements::kBlockbusterChampionId, achievements::kShallowWorldviewId, achievements::kYeAreIdleId, achievements::kNoEnemiesId, achievements::kTacticallyCenteredId })
    {
        CAPTURE(id);
        CHECK_FALSE(unlocked.contains(id));
    }
    // Facts read at the victory itself still count.
    context.victoryType = 2;
    context.year        = -2000;
    CHECK(unlockedBy(context).contains(achievements::kCosmicEclipseId));
}

TEST_CASE("custom_achievement_victory_evaluates_every_predicate", "[ctest][custom-achievements]")
{
    // One Deity win can satisfy many achievements. Each unlocks once.
    auto context              = withHandicaps(tracked(2), 200);
    context.year              = -3500;
    context.occEnabled        = true;
    context.occFromSetup      = true;
    context.gotwGame          = true;
    context.gotwWeek          = 36428;
    context.firstFoundingTurn = 100;
    const std::set<uint32_t> expected{ achievements::kConcentratedPowerOfWillId,
                                       achievements::kOneToRuleId,
                                       achievements::kBlockbusterChampionId,
                                       achievements::kShallowWorldviewId,
                                       achievements::kCosmicEclipseId,
                                       achievements::kWeekendWarriorId,
                                       achievements::kYeAreIdleId,
                                       achievements::kLeapThroughTimeId,
                                       achievements::kNoEnemiesId };
    CHECK(unlockedBy(context) == expected);

    std::set<uint32_t> unlocked;
    int                unlockCalls = 0;
    const auto         award       = [&]
    {
        return achievements::AwardVictory(
            context,
            [&unlocked](uint32_t id)
            {
                return unlocked.contains(id);
            },
            [&](uint32_t id)
            {
                ++unlockCalls;
                return unlocked.insert(id).second;
            });
    };
    const auto first = award();
    CHECK(first.size() == 12);
    CHECK(unlocked == expected);
    CHECK(unlockCalls == static_cast<int>(expected.size()));
    uint32_t previous = 0;
    for (const auto& decision : first)
    {
        CHECK(decision.id > previous);
        previous = decision.id;
        CHECK(decision.qualifies == expected.contains(decision.id));
        CHECK(decision.result == (decision.qualifies ? achievements::AwardResult::Unlocked : achievements::AwardResult::NotQualified));
    }

    for (const auto& decision : award())
    {
        CHECK(decision.result == (decision.qualifies ? achievements::AwardResult::AlreadyUnlocked : achievements::AwardResult::NotQualified));
    }
    CHECK(unlockCalls == static_cast<int>(expected.size()));

    // A failing store reports Failed for each qualifying achievement and nothing else.
    for (const auto& decision : achievements::AwardVictory(
             context,
             [](uint32_t)
             {
                 return false;
             },
             [](uint32_t)
             {
                 return false;
             }))
    {
        CHECK(decision.result == (decision.qualifies ? achievements::AwardResult::Failed : achievements::AwardResult::NotQualified));
    }

    // A network game unlocks nothing.
    context.networkGame = true;
    CHECK(unlockedBy(context).empty());
}

TEST_CASE("custom_achievement_victory_keeps_the_original_pair", "[ctest][custom-achievements]")
{
    // Award and AwardOneToRule remain the 0x10001 and 0x10002 decisions of a victory.
    for (const bool occ : { false, true })
    {
        auto context         = qualifying();
        context.occEnabled   = occ;
        context.occFromSetup = occ;
        const auto unlocked  = unlockedBy(context);
        CHECK(unlocked.contains(achievements::kConcentratedPowerOfWillId));
        CHECK(unlocked.contains(achievements::kOneToRuleId) == occ);
        // No tracked history and no readable year.
        CHECK(unlocked.size() == (occ ? 2u : 1u));
    }
}

TEST_CASE("custom_achievement_duplicate_victory_delivery_notifies_once", "[ctest][custom-achievements]")
{
    rex::system::AchievementManager manager;
    for (const auto& info : achievements::AllAchievements())
    {
        REQUIRE(manager.RegisterAchievement(info));
    }
    int notifications = 0;
    manager.RegisterNotificationCallback(
        [&notifications](const rex::system::AchievementEvent&)
        {
            ++notifications;
        });
    // 100% handicaps in 2000 AD qualify for Reason to Remember, Shallow Worldview and I Have No Enemies.
    const auto context = withHandicaps(tracked(), 100);
    const auto award   = [&]
    {
        return achievements::AwardVictory(
            context,
            [&manager](uint32_t id)
            {
                return manager.IsUnlocked(id);
            },
            [&manager](uint32_t id)
            {
                return manager.UnlockAchievement(id, rex::system::AchievementNotification::kShow) ==
                       rex::system::AchievementUnlockResult::kUnlocked;
            });
    };
    int qualified = 0;
    for (const auto& decision : award())
    {
        qualified += decision.qualifies;
        CHECK(decision.result == (decision.qualifies ? achievements::AwardResult::Unlocked : achievements::AwardResult::NotQualified));
    }
    CHECK(qualified == 3);
    CHECK(notifications == 3);
    for (const auto& decision : award())
    {
        CHECK(decision.result == (decision.qualifies ? achievements::AwardResult::AlreadyUnlocked : achievements::AwardResult::NotQualified));
    }
    CHECK(notifications == 3);
}

TEST_CASE("custom_achievement_templar_discovery_year", "[ctest][custom-achievements]")
{
    const auto discover = [](int32_t year)
    {
        return eventAward(
            [year](const auto& isUnlocked, const auto& unlock)
            {
                return achievements::RecordTemplarDiscovery(year, isUnlocked, unlock);
            });
    };
    CHECK(discover(-3500) == achievements::AwardResult::Unlocked);
    CHECK(discover(-4000) == achievements::AwardResult::Unlocked);
    CHECK(discover(-3499) == achievements::AwardResult::NotQualified);
    CHECK(discover(-3400) == achievements::AwardResult::NotQualified);

    std::set<uint32_t> unlocked;
    const auto         isUnlocked = [&unlocked](uint32_t id)
    {
        return unlocked.contains(id);
    };
    const auto unlock = [&unlocked](uint32_t id)
    {
        return unlocked.insert(id).second;
    };
    CHECK(achievements::RecordTemplarDiscovery(-3500, isUnlocked, unlock) == achievements::AwardResult::Unlocked);
    CHECK(achievements::RecordTemplarDiscovery(-3500, isUnlocked, unlock) == achievements::AwardResult::AlreadyUnlocked);
    CHECK(unlocked == std::set<uint32_t>{ achievements::kHalcyonTemplarId });
}

TEST_CASE("custom_achievement_tile_distance", "[ctest][custom-achievements]")
{
    using achievements::Tile;
    CHECK(achievements::TileDistance({ 0, 10 }, { 31, 10 }) == 1);
    CHECK(achievements::TileDistance({ 31, 10 }, { 0, 10 }) == 1);
    CHECK(achievements::TileDistance({ 4, 3 }, { 20, 27 }) == 24);
    CHECK(achievements::TileDistance({ 4, 3 }, { 20, 26 }) == 23);
    CHECK(achievements::TileDistance({ 5, 5 }, { 5, 5 }) == 0);
    CHECK(achievements::TileDistance({ 0, 0 }, { 16, 0 }) == 16);
    // y does not wrap.
    CHECK(achievements::TileDistance({ 7, 0 }, { 7, 31 }) == 31);
    CHECK(achievements::TileDistance({ 0, 0 }, { 31, 31 }) == 31);
    for (const Tile bad : { Tile{ -1, 5 }, Tile{ 5, -1 }, Tile{ 32, 5 }, Tile{ 5, 32 }, Tile{} })
    {
        CAPTURE(bad.x, bad.y);
        CHECK(achievements::TileDistance(bad, { 5, 5 }) == -1);
        CHECK(achievements::TileDistance({ 5, 5 }, bad) == -1);
    }
}

TEST_CASE("custom_achievement_icbm_distance_threshold", "[ctest][custom-achievements]")
{
    using achievements::Tile;
    const auto launch = [](Tile source, Tile target)
    {
        return eventAward(
            [&](const auto& isUnlocked, const auto& unlock)
            {
                return achievements::RecordIcbmLaunch(source, target, isUnlocked, unlock);
            });
    };
    CHECK(launch({ 4, 3 }, { 20, 27 }) == achievements::AwardResult::Unlocked);
    CHECK(launch({ 4, 3 }, { 20, 26 }) == achievements::AwardResult::NotQualified);
    CHECK(launch({ 0, 0 }, { 0, 24 }) == achievements::AwardResult::Unlocked);
    CHECK(launch({ 0, 0 }, { 0, 23 }) == achievements::AwardResult::NotQualified);
    // The wrap makes the far edge adjacent.
    CHECK(launch({ 0, 10 }, { 31, 10 }) == achievements::AwardResult::NotQualified);
    CHECK(launch({ -1, 0 }, { 24, 24 }) == achievements::AwardResult::NotQualified);
    CHECK(launch({ 0, 0 }, { 32, 24 }) == achievements::AwardResult::NotQualified);
}

TEST_CASE("custom_achievement_capital_capture_rules", "[ctest][custom-achievements]")
{
    const auto capture = [](int32_t year, int32_t difficulty, bool network)
    {
        return eventAward(
            [&](const auto& isUnlocked, const auto& unlock)
            {
                return achievements::RecordCapitalCapture(year, difficulty, network, isUnlocked, unlock);
            });
    };
    CHECK(capture(-3600, 4, false) == achievements::AwardResult::Unlocked);
    CHECK(capture(-3501, 4, false) == achievements::AwardResult::Unlocked);
    CHECK(capture(-3500, 4, false) == achievements::AwardResult::NotQualified);
    CHECK(capture(-3400, 4, false) == achievements::AwardResult::NotQualified);
    CHECK(capture(-3600, 3, false) == achievements::AwardResult::NotQualified);
    CHECK(capture(-3600, 5, false) == achievements::AwardResult::Unlocked);
    CHECK(capture(-3600, 4, true) == achievements::AwardResult::NotQualified);
}

TEST_CASE("custom_achievement_last_stand_strength", "[ctest][custom-achievements]")
{
    CHECK(achievements::AgainstTheOdds(11, 10));
    CHECK_FALSE(achievements::AgainstTheOdds(10, 10));
    CHECK_FALSE(achievements::AgainstTheOdds(9, 10));
    CHECK(achievements::AgainstTheOdds(1, 0));

    const auto stand = [](int64_t attack, int64_t defense)
    {
        return eventAward(
            [&](const auto& isUnlocked, const auto& unlock)
            {
                return achievements::RecordLastStand(attack, defense, isUnlocked, unlock);
            });
    };
    CHECK(stand(11, 10) == achievements::AwardResult::Unlocked);
    CHECK(stand(10, 10) == achievements::AwardResult::NotQualified);
    CHECK(stand(9, 10) == achievements::AwardResult::NotQualified);
}

TEST_CASE("custom_achievement_deep_ocean", "[ctest][custom-achievements]")
{
    using achievements::Tile;
    constexpr uint8_t kLand = 3;

    SECTION("open sea is deep")
    {
        const auto terrain = openSea();
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 10, 10 }));
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 0, 0 }));
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 31, 31 }));
    }

    SECTION("any non-ocean tile or neighbour disqualifies")
    {
        auto terrain = openSea();
        setTile(terrain, 10, 10, 1);
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 10, 10 }));
        // Tiles next to land are not deep. Land two tiles away does not matter.
        terrain = openSea();
        setTile(terrain, 11, 11, kLand);
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                if (dx != 0 || dy != 0)
                {
                    CAPTURE(dx, dy);
                    CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 11 + dx, 11 + dy }));
                }
            }
        }
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 9, 11 }));
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 11, 13 }));
    }

    SECTION("x wraps through the map width")
    {
        auto terrain = openSea();
        setTile(terrain, 31, 10, kLand);
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 0, 10 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 0, 11 }));
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 1, 10 }));
        // A narrower map wraps at its own width and ignores columns beyond it.
        terrain = openSea();
        setTile(terrain, 9, 4, kLand);
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 10, 32, { 0, 4 }));
        terrain = openSea();
        setTile(terrain, 31, 4, kLand);
        CHECK(achievements::IsDeepOcean(terrain, 10, 32, { 0, 4 }));
    }

    SECTION("y clips at the edges instead of wrapping")
    {
        auto terrain = openSea();
        setTile(terrain, 5, 31, kLand);
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 5, 0 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 5, 30 }));
        terrain = openSea();
        setTile(terrain, 5, 0, kLand);
        CHECK(achievements::IsDeepOcean(terrain, 32, 32, { 5, 31 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, { 5, 1 }));
        // A shorter map clips at its own height.
        terrain = openSea();
        setTile(terrain, 5, 20, kLand);
        CHECK(achievements::IsDeepOcean(terrain, 32, 20, { 5, 19 }));
    }

    SECTION("out of bounds and short input are false")
    {
        const auto terrain = openSea();
        for (const Tile tile : { Tile{ -1, 5 }, Tile{ 5, -1 }, Tile{ 32, 5 }, Tile{ 5, 32 }, Tile{} })
        {
            CAPTURE(tile.x, tile.y);
            CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 32, tile));
        }
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 0, 32, { 0, 0 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 0, { 0, 0 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 41, 32, { 5, 5 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 32, 41, { 5, 5 }));
        CHECK_FALSE(achievements::IsDeepOcean(std::span(terrain).first(1599), 32, 32, { 5, 5 }));
        CHECK_FALSE(achievements::IsDeepOcean({}, 32, 32, { 5, 5 }));
        // A tile outside the current map is out of bounds even inside the plane.
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 20, 20, { 25, 5 }));
        CHECK_FALSE(achievements::IsDeepOcean(terrain, 20, 20, { 5, 25 }));
    }
}

TEST_CASE("custom_achievement_history_records", "[ctest][custom-achievements]")
{
    const ProgressReset reset;

    SECTION("untracked games ignore every observation")
    {
        achievements::ResetProgress();
        achievements::RecordUnitBuilt();
        achievements::RecordBattle();
        achievements::RecordDeepOceanEntry();
        achievements::RecordCityFounded(5);
        CHECK(achievements::CurrentProgress() == achievements::Progress{});
    }

    SECTION("tracked games record each disqualifier")
    {
        achievements::BeginNewGame(false, false);
        CHECK_FALSE(achievements::CurrentProgress().unitBuilt);
        achievements::RecordUnitBuilt();
        CHECK(achievements::CurrentProgress().unitBuilt);
        CHECK_FALSE(achievements::CurrentProgress().battleFought);
        achievements::RecordBattle();
        CHECK(achievements::CurrentProgress().battleFought);
        CHECK_FALSE(achievements::CurrentProgress().deepOceanEntered);
        achievements::RecordDeepOceanEntry();
        CHECK(achievements::CurrentProgress().deepOceanEntered);
        achievements::RecordUnitBuilt();
        CHECK(achievements::CurrentProgress().unitBuilt);
    }

    SECTION("the first founding turn is kept")
    {
        achievements::BeginNewGame(false, false);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == -1);
        achievements::RecordCityFounded(-1);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == -1);
        achievements::RecordCityFounded(120);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == 120);
        achievements::RecordCityFounded(3);
        achievements::RecordCityFounded(0);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == 120);
    }

    SECTION("turn zero counts as the first founding")
    {
        achievements::BeginNewGame(false, false);
        achievements::RecordCityFounded(0);
        achievements::RecordCityFounded(100);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == 0);
    }

    SECTION("a huge turn is bounded so the sidecar stays writable")
    {
        achievements::BeginNewGame(false, false);
        achievements::RecordCityFounded(INT32_MAX);
        CHECK(achievements::CurrentProgress().firstFoundingTurn == achievements::kMaximumRecordedTurn);
    }
}

TEST_CASE("custom_achievement_new_game_and_load_resets", "[ctest][custom-achievements]")
{
    const ProgressReset reset;

    achievements::BeginNewGame(true, true);
    auto progress = achievements::CurrentProgress();
    CHECK(progress.occFromSetup);
    CHECK(progress.historyKnown);
    CHECK(progress.gotwGame);
    CHECK(progress.gotwWeek == 0);
    CHECK(progress.initialAiOpponents == -1);
    CHECK(progress.firstFoundingTurn == -1);
    CHECK_FALSE(progress.unitBuilt);

    achievements::RecordSetupOutcome(6, 36428);
    progress = achievements::CurrentProgress();
    CHECK(progress.initialAiOpponents == 6);
    CHECK(progress.gotwWeek == 36428);

    FakeStore store;
    achievements::RecordUnitBuilt();
    achievements::RecordBattle();
    achievements::RecordDeepOceanEntry();
    achievements::RecordCityFounded(7);
    store.recordDefeat();
    CHECK(achievements::CurrentProgress().barbarianDefeats == 1);

    // A new accepted setup discards everything from the previous game.
    achievements::BeginNewGame(false, false);
    CHECK(achievements::CurrentProgress() == achievements::Progress{ false, 0, true });

    // A rejected setup or a load without a sidecar clears to unknown.
    achievements::RecordSetupOutcome(5, 99);
    CHECK(achievements::CurrentProgress().initialAiOpponents == 5);
    CHECK(achievements::CurrentProgress().gotwWeek == 0);
    achievements::RecordUnitBuilt();
    achievements::ResetProgress();
    progress = achievements::CurrentProgress();
    CHECK(progress == achievements::Progress{});
    CHECK_FALSE(progress.historyKnown);
    CHECK(progress.initialAiOpponents == -1);
    CHECK_FALSE(progress.gotwGame);
    CHECK(progress.gotwWeek == 0);
}

TEST_CASE("custom_achievement_setup_outcome_bounds", "[ctest][custom-achievements]")
{
    const ProgressReset reset;

    // Untracked games take no setup facts.
    achievements::ResetProgress();
    achievements::RecordSetupOutcome(6, 36428);
    CHECK(achievements::CurrentProgress() == achievements::Progress{});

    achievements::BeginNewGame(false, false);
    achievements::RecordSetupOutcome(6, 36428);
    CHECK(achievements::CurrentProgress().initialAiOpponents == 6);
    // Only a Game of the Week keeps a week.
    CHECK(achievements::CurrentProgress().gotwWeek == 0);
    for (const int32_t count : { -1, -5, 7, 100 })
    {
        CAPTURE(count);
        achievements::RecordSetupOutcome(count, 0);
        CHECK(achievements::CurrentProgress().initialAiOpponents == -1);
    }
    achievements::RecordSetupOutcome(0, 0);
    CHECK(achievements::CurrentProgress().initialAiOpponents == 0);
}

TEST_CASE("custom_achievement_restore_progress_normalizes", "[ctest][custom-achievements]")
{
    const ProgressReset reset;
    achievements::BeginNewGame(false, true);
    achievements::RecordSetupOutcome(4, 36428);

    achievements::Progress saved;
    saved.occFromSetup       = true;
    saved.barbarianDefeats   = 12;
    saved.historyKnown       = true;
    saved.unitBuilt          = true;
    saved.battleFought       = true;
    saved.deepOceanEntered   = true;
    saved.firstFoundingTurn  = 101;
    saved.initialAiOpponents = 6;
    saved.gotwGame           = true;
    saved.gotwWeek           = 36428;
    achievements::RestoreProgress(saved);
    // A load never keeps Game of the Week state.
    saved.gotwGame = false;
    saved.gotwWeek = 0;
    CHECK(achievements::CurrentProgress() == saved);

    saved.firstFoundingTurn  = -7;
    saved.initialAiOpponents = 40;
    achievements::RestoreProgress(saved);
    CHECK(achievements::CurrentProgress().firstFoundingTurn == -1);
    CHECK(achievements::CurrentProgress().initialAiOpponents == -1);
    saved.firstFoundingTurn = INT32_MAX;
    achievements::RestoreProgress(saved);
    CHECK(achievements::CurrentProgress().firstFoundingTurn == achievements::kMaximumRecordedTurn);

    // Restored history keeps recording without replacing the first founding turn.
    saved.firstFoundingTurn = 130;
    achievements::RestoreProgress(saved);
    achievements::RecordCityFounded(10);
    CHECK(achievements::CurrentProgress().firstFoundingTurn == 130);
}
