#include "gameplay_fixes.h"
#include "gotw_practice.h"
#include "gotw_settings.h"
#include "host_settings.h"
#include "title_text.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

namespace gotw = rerevved::gotw;
namespace host = rerevved::host_settings;
namespace tt   = rerevved::title_text;

struct CleanGame
{
    CleanGame()
    {
        gotw::ArmLoadIsolation(false);
        gotw::ResetForLoad();
    }

    ~CleanGame()
    {
        gotw::ArmLoadIsolation(false);
        gotw::ResetForLoad();
    }
};

host::HostSettings practiceSettings()
{
    host::HostSettings settings;
    settings.gotwAllowSaving = true;
    return settings;
}

gotw::PracticeBlock automaticBlock()
{
    return { true, false, false, 1085, 277, 4, 10 };
}

gotw::PracticeBlock customBlock()
{
    return { false, true, true, 3, gotw::SeedForWeek(3), 9, 5 };
}

std::vector<uint8_t> encoded(const gotw::PracticeBlock& block)
{
    std::vector<uint8_t> bytes;
    gotw::AppendPracticeBlock(bytes, block);
    return bytes;
}

std::optional<gotw::PracticeBlock> decoded(const std::vector<uint8_t>& bytes)
{
    size_t     offset = 0;
    const auto block  = gotw::ReadPracticeBlock(bytes, offset);
    if (block)
    {
        CHECK(offset == bytes.size());
    }
    return block;
}

} // namespace

TEST_CASE("gotw_practice_block_round_trips", "[ctest][gotw-practice]")
{
    const auto fixed = encoded(automaticBlock());
    REQUIRE(fixed.size() == gotw::kPracticeBlockSize);
    CHECK(decoded(fixed) == automaticBlock());

    const auto bytes = encoded(customBlock());
    REQUIRE(bytes.size() == gotw::kPracticeBlockSize);
    CHECK(decoded(bytes) == customBlock());
    // Big endian week/seed words, then effective civilization and Earliest Domination bytes.
    CHECK(bytes[1] == 0);
    CHECK(bytes[4] == 3);
    CHECK(bytes[8] == gotw::SeedForWeek(3));
    CHECK(bytes[9] == 9);
    CHECK(bytes[10] == 5);
}

TEST_CASE("gotw_practice_block_rejects_unproven_metadata", "[ctest][gotw-practice]")
{
    SECTION("an invalid block value is not valid")
    {
        auto block = automaticBlock();
        block.seed = 0;
        CHECK_FALSE(gotw::IsValid(block));
        block      = automaticBlock();
        block.week = 0;
        CHECK_FALSE(gotw::IsValid(block));
        block              = customBlock();
        block.civilization = 16;
        CHECK_FALSE(gotw::IsValid(block));
        block      = customBlock();
        block.seed = 1;
        CHECK_FALSE(gotw::IsValid(block));
        block                   = automaticBlock();
        block.dominationMinimum = 7;
        CHECK_FALSE(gotw::IsValid(block));
    }
    SECTION("a short block and unknown flags are rejected")
    {
        const auto bytes = encoded(customBlock());
        auto       copy  = bytes;
        for (size_t length = 0; length < bytes.size(); ++length)
        {
            copy.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
            CHECK_FALSE(decoded(copy));
        }
        copy    = bytes;
        copy[0] = static_cast<uint8_t>(copy[0] | 8);
        CHECK_FALSE(decoded(copy));
    }
    SECTION("a zero seed or an unrecorded civilization is rejected")
    {
        const auto bytes = encoded(automaticBlock());
        auto       copy  = bytes;
        for (size_t i = 5; i < 9; ++i)
        {
            copy[i] = 0;
        }
        CHECK_FALSE(decoded(copy));
        copy    = bytes;
        copy[9] = 16;
        CHECK_FALSE(decoded(copy));
    }
}

TEST_CASE("gotw_practice_latches_the_permission_at_the_start_of_the_attempt", "[ctest][gotw-practice]")
{
    CleanGame clean;
    SECTION("permission on at the start makes the whole attempt practice")
    {
        const auto latched = gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK(latched.practice);
        CHECK_FALSE(latched.IsCustom());
        CHECK(gotw::ForGame().practice);
    }
    SECTION("permission off is an ordinary attempt")
    {
        CHECK_FALSE(gotw::LatchForGame(1085, 0x1000, host::HostSettings{}).practice);
    }
    SECTION("the next attempt latches its own permission")
    {
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK_FALSE(gotw::LatchForGame(1085, 0x1000, host::HostSettings{}).practice);
        CHECK_FALSE(gotw::ForGame().practice);
    }
    SECTION("a later nonweekly game ends the practice origin and any unverified isolation")
    {
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        gotw::EndPractice();
        CHECK_FALSE(gotw::ForGame().practice);
        gotw::ArmLoadIsolation(true);
        gotw::ResetForLoad();
        REQUIRE(gotw::ForGame().isolated);
        gotw::EndPractice();
        CHECK_FALSE(gotw::ForGame().Isolates());
    }
    SECTION("network settings never carry the permission")
    {
        CHECK_FALSE(host::Normalize(practiceSettings(), true).gotwAllowSaving);
        CHECK(host::Normalize(practiceSettings(), false).gotwAllowSaving);
    }
}

TEST_CASE("gotw_practice_load_restores_a_saved_game_and_clears_stale_state", "[ctest][gotw-practice]")
{
    CleanGame clean;
    gotw::FreezeStandardRules(2, true);
    gotw::LatchForGame(1085, 0x1000, practiceSettings());
    SECTION("a load resets every latch before the saved policy is restored")
    {
        gotw::ResetForLoad();
        const auto cleared = gotw::ForGame();
        CHECK_FALSE(cleared.practice);
        CHECK_FALSE(cleared.IsCustom());
        CHECK_FALSE(cleared.seedPending);
        CHECK_FALSE(cleared.isolated);
        CHECK_FALSE(gotw::StandardRulesForGame());
        CHECK_FALSE(gotw::CurrentPracticeBlock(4, 10));
    }
    SECTION("a practice restore keeps the loaded origin and the saved policy")
    {
        gotw::ResetForLoad();
        gotw::RestorePractice(customBlock());
        const auto restored = gotw::ForGame();
        CHECK(restored.practice);
        CHECK(restored.customWeek);
        CHECK(restored.customCivilization);
        CHECK(restored.week == 3);
        CHECK(restored.seed == gotw::SeedForWeek(3));
        CHECK(restored.civilization == 9);
        CHECK_FALSE(gotw::StandardRulesForGame());
        CHECK(gotw::CurrentPracticeBlock(9, 5) == customBlock());
        gotw::RestorePractice(automaticBlock());
        CHECK(gotw::StandardRulesForGame());
        CHECK(gotw::ForGame().civilization == 4);
        CHECK_FALSE(gotw::ForGame().customCivilization);
        CHECK(gotw::CurrentPracticeBlock(4, 10) == automaticBlock());
    }
    SECTION("the saved seed reaches the load writer once for the saved week")
    {
        gotw::ResetForLoad();
        gotw::RestorePractice(automaticBlock());
        CHECK(gotw::TakeResumeSeed(1085) == 277);
        CHECK_FALSE(gotw::TakeResumeSeed(1085));
    }
    SECTION("a different saved week gets no seed")
    {
        gotw::ResetForLoad();
        gotw::RestorePractice(automaticBlock());
        CHECK_FALSE(gotw::TakeResumeSeed(1086));
        CHECK_FALSE(gotw::TakeResumeSeed(1085));
    }
    SECTION("an ordinary load has nothing to supply")
    {
        gotw::ResetForLoad();
        CHECK_FALSE(gotw::TakeResumeSeed(1085));
    }
}

TEST_CASE("gotw_practice_block_comes_only_from_a_valid_practice_game", "[ctest][gotw-practice]")
{
    CleanGame clean;
    SECTION("an ordinary game has no block")
    {
        gotw::LatchForGame(1085, 0x1000, host::HostSettings{});
        CHECK_FALSE(gotw::CurrentPracticeBlock(4, 10));
    }
    SECTION("the effective week and seed are captured once the native seed is known")
    {
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        // Native seed is unknown until recorded after the launch writer.
        CHECK_FALSE(gotw::CurrentPracticeBlock(4, 10));
        gotw::RecordAutomaticSeed(277);
        gotw::FreezeStandardRules(2, true);
        CHECK(gotw::CurrentPracticeBlock(4, 10) == gotw::PracticeBlock{ true, false, false, 1085, 277, 4, 10 });
    }
    SECTION("a custom week and civilization are captured with the standard rules latch")
    {
        auto settings                   = practiceSettings();
        settings.gotwCustomWeek         = true;
        settings.gotwWeek               = 3;
        settings.gotwCustomCivilization = true;
        settings.gotwCivilization       = 9;
        gotw::FreezeStandardRules(2, false);
        gotw::LatchForGame(1085, 0x1000, settings);
        CHECK(gotw::CurrentPracticeBlock(9, 5) == customBlock());
    }
    SECTION("the effective civilization must be a real one")
    {
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        gotw::RecordAutomaticSeed(277);
        CHECK_FALSE(gotw::CurrentPracticeBlock(16, 10));
        CHECK_FALSE(gotw::CurrentPracticeBlock(4, 7));
    }
}

TEST_CASE("gotw_practice_load_classification_fails_closed", "[ctest][gotw-practice]")
{
    using gotw::LoadKind;
    const auto saved = std::optional{ automaticBlock() };
    SECTION("a validated practice save loads as practice")
    {
        CHECK(gotw::ClassifyLoad(1085, saved, false, true) == LoadKind::Practice);
    }
    SECTION("a block that cannot resume is isolated")
    {
        CHECK(gotw::ClassifyLoad(1086, saved, false, true) == LoadKind::Isolated);
        CHECK(gotw::ClassifyLoad(0, saved, false, true) == LoadKind::Isolated);
        CHECK(gotw::ClassifyLoad(1085, saved, false, false) == LoadKind::Isolated);
    }
    SECTION("the native week alone is not an origin marker")
    {
        CHECK(gotw::ClassifyLoad(1085, std::nullopt, false, true) == LoadKind::Ordinary);
        CHECK(gotw::ClassifyLoad(1085, std::nullopt, false, false) == LoadKind::Ordinary);
        CHECK(gotw::ClassifyLoad(0, std::nullopt, false, true) == LoadKind::Ordinary);
    }
    SECTION("a schema 3 sidecar that was rejected or digest-mismatched still claims a block")
    {
        CHECK(gotw::ClassifyLoad(0, std::nullopt, true, true) == LoadKind::Isolated);
        CHECK(gotw::ClassifyLoad(1085, std::nullopt, true, true) == LoadKind::Isolated);
    }
    SECTION("a multiplayer load never resumes but is still isolated")
    {
        CHECK(gotw::ClassifyLoad(1085, saved, false, false) == LoadKind::Isolated);
        CHECK(gotw::ClassifyLoad(0, std::nullopt, true, false) == LoadKind::Isolated);
    }
    SECTION("only the proved single-player setup modes may resume")
    {
        for (const int32_t mode : { 1, 2, 3 })
        {
            CHECK(gotw::IsSinglePlayerSetupMode(mode));
        }
        for (const int32_t mode : { -1, 0, 4, 5, 6, 7, 255 })
        {
            CHECK_FALSE(gotw::IsSinglePlayerSetupMode(mode));
        }
    }
}

TEST_CASE("gotw_practice_every_load_is_classified_never_refused", "[ctest][gotw-practice]")
{
    using gotw::LoadKind;
    const auto saved = std::optional{ automaticBlock() };
    SECTION("every combination of origin, block and path yields a kind")
    {
        for (const uint32_t week : { 0u, 1085u })
        {
            for (const bool block : { false, true })
            {
                for (const bool claim : { false, true })
                {
                    for (const bool writer : { false, true })
                    {
                        const auto kind = gotw::ClassifyLoad(week, block ? saved : std::nullopt, claim, writer);
                        CHECK((kind == LoadKind::Ordinary || kind == LoadKind::Practice || kind == LoadKind::Isolated));
                        CHECK((kind == LoadKind::Practice) == (week == 1085 && block && writer));
                        CHECK((kind == LoadKind::Ordinary) == (!block && !claim));
                    }
                }
            }
        }
    }
    SECTION("a practice game does not change how a save is classified")
    {
        CleanGame clean;
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK(gotw::ClassifyLoad(0, std::nullopt, false, true) == LoadKind::Ordinary);
        CHECK(gotw::ClassifyLoad(1085, std::nullopt, false, true) == LoadKind::Ordinary);
        CHECK(gotw::ClassifyLoad(1085, saved, false, true) == LoadKind::Practice);
    }
}

TEST_CASE("gotw_practice_load_isolation_survives_the_reset_and_a_failed_restore", "[ctest][gotw-practice]")
{
    CleanGame clean;
    gotw::LatchForGame(1085, 0x1000, practiceSettings());
    SECTION("an armed isolation is kept by the reset and isolates every result surface")
    {
        gotw::ArmLoadIsolation(true);
        gotw::ResetForLoad();
        const auto loaded = gotw::ForGame();
        CHECK_FALSE(loaded.practice);
        CHECK(loaded.isolated);
        for (int32_t mode : { 1, 2, 3 })
        {
            CHECK(gotw::IsolatesWeeklyResult(loaded, mode));
        }
        CHECK(gotw::IsolateWeeklyResult(3));
    }
    SECTION("the isolation is consumed by one reset")
    {
        gotw::ArmLoadIsolation(true);
        gotw::ResetForLoad();
        gotw::ResetForLoad();
        CHECK_FALSE(gotw::ForGame().Isolates());
    }
    SECTION("an ordinary load or a load that never reached the check isolates nothing")
    {
        gotw::ArmLoadIsolation(false);
        gotw::ResetForLoad();
        CHECK_FALSE(gotw::ForGame().Isolates());
        CHECK_FALSE(gotw::IsolateWeeklyResult(3));
    }
    SECTION("a validated restore replaces the unverified isolation with the practice origin")
    {
        gotw::ArmLoadIsolation(true);
        gotw::ResetForLoad();
        gotw::RestorePractice(automaticBlock());
        const auto restored = gotw::ForGame();
        CHECK(restored.practice);
        CHECK_FALSE(restored.isolated);
        CHECK(restored.Isolates());
    }
}

TEST_CASE("gotw_practice_isolates_every_weekly_result_surface", "[ctest][gotw-practice]")
{
    const gotw::Selection ordinary;
    gotw::Selection       custom;
    custom.customWeek = true;
    gotw::Selection practice;
    practice.practice = true;
    gotw::Selection isolated;
    isolated.isolated = true;
    SECTION("the practice origin and an unverified origin isolate in every mode")
    {
        for (int32_t mode : { 0, 1, 2, 3, 4, 6 })
        {
            CHECK(gotw::IsolatesWeeklyResult(practice, mode));
            CHECK(gotw::IsolatesWeeklyResult(isolated, mode));
        }
    }
    SECTION("a custom selection still isolates only in mode 2")
    {
        CHECK(gotw::IsolatesWeeklyResult(custom, 2));
        CHECK_FALSE(gotw::IsolatesWeeklyResult(custom, 3));
    }
    SECTION("an ordinary game isolates nothing")
    {
        for (int32_t mode : { 1, 2, 3 })
        {
            CHECK_FALSE(gotw::IsolatesWeeklyResult(ordinary, mode));
        }
    }
    SECTION("the latch alone isolates, so a prefetch completing during teardown cannot write the caches")
    {
        CleanGame clean;
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK(gotw::IsolateWeeklyResult(3));
        gotw::ResetForLoad();
        CHECK_FALSE(gotw::IsolateWeeklyResult(3));
    }
    SECTION("the live predicate follows the latched game")
    {
        CleanGame clean;
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK(gotw::IsolateWeeklyResult(2));
        CHECK(gotw::IsolateWeeklyResult(3));
        gotw::LatchForGame(1085, 0x1000, host::HostSettings{});
        CHECK_FALSE(gotw::IsolateWeeklyResult(2));
    }
}

TEST_CASE("gotw_practice_earliest_domination_is_restored_with_the_practice_policy", "[ctest][gotw-practice]")
{
    namespace fixes = rerevved::gameplay_fixes;
    fixes::NoteGameLoad();
    CHECK(fixes::RestoredDominationMinimum() == 0);
    fixes::RestoreDominationMinimum(5);
    CHECK(fixes::RestoredDominationMinimum() == 5);
    UNSCOPED_INFO("the next load and the next new game clear it");
    fixes::NoteGameLoad();
    CHECK(fixes::RestoredDominationMinimum() == 0);
    fixes::RestoreDominationMinimum(10);
    fixes::NoteNewGame();
    CHECK(fixes::RestoredDominationMinimum() == 0);
    fixes::NoteGameLoad();
}

TEST_CASE("gotw_practice_menu_and_load_guard_decisions", "[ctest][gotw-practice]")
{
    gotw::Selection practice;
    practice.practice = true;
    SECTION("the pause menu keeps Save and Load only for a practice attempt in mode 2")
    {
        CHECK(gotw::AllowsPracticeMenu(practice, 2));
        CHECK_FALSE(gotw::AllowsPracticeMenu(practice, 1));
        CHECK_FALSE(gotw::AllowsPracticeMenu(practice, 3));
        CHECK_FALSE(gotw::AllowsPracticeMenu(gotw::Selection{}, 2));
        gotw::Selection custom;
        custom.customWeek = true;
        CHECK_FALSE(gotw::AllowsPracticeMenu(custom, 2));
        gotw::Selection isolated;
        isolated.isolated = true;
        CHECK_FALSE(gotw::AllowsPracticeMenu(isolated, 2));
    }
}

TEST_CASE("gotw_practice_setting_text_exists_in_every_language", "[ctest][gotw-practice]")
{
    const std::array<tt::Language, 5> languages{ tt::Language::kEnglish, tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian };
    for (const auto* field : { "label", "tip" })
    {
        const auto* row = tt::FindRow(tt::SettingKey("gotw_allow_saving", field));
        REQUIRE(row != nullptr);
        for (const auto language : languages)
        {
            const auto text = row->text[static_cast<size_t>(language)];
            CAPTURE(field, static_cast<int>(language));
            CHECK_FALSE(text.empty());
            if (language != tt::Language::kEnglish)
            {
                CHECK(text != row->text[0]);
            }
        }
    }
}

TEST_CASE("gotw_practice_a_resave_of_an_isolated_game_keeps_the_isolation_marker", "[ctest][gotw-practice]")
{
    CleanGame clean;
    SECTION("an ordinary game writes no marker")
    {
        gotw::LatchForGame(1085, 0x1000, host::HostSettings{});
        CHECK_FALSE(gotw::NeedsIsolationMarker(false));
    }
    SECTION("a practice game marks every save that carries no block")
    {
        gotw::LatchForGame(1085, 0x1000, practiceSettings());
        CHECK(gotw::NeedsIsolationMarker(false));
        CHECK_FALSE(gotw::NeedsIsolationMarker(true));
    }
    SECTION("a game that loaded isolated, not as practice, marks its next save")
    {
        gotw::ArmLoadIsolation(true);
        gotw::ResetForLoad();
        REQUIRE(gotw::ForGame().isolated);
        REQUIRE_FALSE(gotw::ForGame().practice);
        CHECK(gotw::NeedsIsolationMarker(false));
        gotw::EndPractice();
        CHECK_FALSE(gotw::NeedsIsolationMarker(false));
    }
}
