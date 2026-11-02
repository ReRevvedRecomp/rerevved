#include "ai_civilization_setup.h"
#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"
#include "guest_import_doubles.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

bool AdmitFirstDiscoveryTempleCity(PPCRegister& buildingIndex);
bool PreserveEgyptianIrrigationReward(
    PPCRegister& player, PPCRegister& technology, PPCRegister& source, PPCRegister& phase);
bool SuppressLiteracyCourthouseReward(PPCRegister& technology, PPCRegister& buildingIndex);
bool SuppressStaleFirstDiscoveryCaption(PPCRegister& savedCity, PPCRegister& cityTable, PPCRegister& acquirer);
bool SkipFailedSpyUnitCreation(PPCRegister& returnedSlot);
bool AdmitAiNuclearOpponent(PPCRegister& player, PPCRegister& opponent, PPCRegister& history);
bool SkipUnlimitedTurnDeadline();
bool SkipTurnLimitWarning();
bool AdmitExtendedTurnLimitWarning();
bool SkipExtendedTurnLimitDeadline();
void ApplyStartingEra(PPCRegister& r31);
void ApplyMultiplayerAiDifficulty(PPCRegister& r11);
void ProjectAlwaysAtWarEarly();
void ProjectAlwaysAtWarGroups();
void ProjectAlwaysAtWarOnLoad();
void RefuseContactChoice(PPCRegister& choice, PPCRegister& p, PPCRegister& q);
bool RefuseRelationRequest(PPCRegister& p, PPCRegister& q, PPCRegister& value);
bool RefuseDeliveredRelation(PPCRegister& p, PPCRegister& q, PPCRegister& value);
REX_EXTERN(sub_82CF0CB0);

namespace gf = rerevved::gameplay_fixes;

TEST_CASE("ai_nuclear_opponent_skips_only_history_bit", "[ctest][gameplay-fixes]")
{
    CHECK(gf::AdmitAiNuclearOpponent(true, 0));
    CHECK(!gf::AdmitAiNuclearOpponent(true, 0x20));
    CHECK(!gf::AdmitAiNuclearOpponent(false, 0));
    CHECK(!gf::AdmitAiNuclearOpponent(false, 0x20));

    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto* memory = runtime.memory();
    for (const uint32_t page : { 0x82F70000u, 0x83120000u })
    {
        auto* heap = memory->LookupHeap(page);
        REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    }
    // Six players with the Barbarians last and player 0 human.
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x82F700AC), 6);
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x82F700B0), 5);
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x8312E608), 0x1);

    PPCRegister player{}, opponent{}, history{};
    opponent.u64 = 0x1234567800000000ull;
    for (const uint32_t p : { 1u, 4u })
    {
        player.u64  = 0x1234567800000000ull | p;
        history.u64 = 0x1234567800000000ull;
        CHECK(AdmitAiNuclearOpponent(player, opponent, history));
        CHECK(player.u64 == (0x1234567800000000ull | p));
        CHECK(opponent.u64 == 0x1234567800000000ull);
        CHECK(history.u64 == 0x1234567800000000ull);
        history.u64 = 0x1234567800000020ull;
        CHECK(!AdmitAiNuclearOpponent(player, opponent, history));
    }
    history.u64 = 0;
    for (const int64_t p : { int64_t{ 0 }, int64_t{ 5 }, int64_t{ 6 }, int64_t{ -1 } })
    {
        player.s64 = p;
        CHECK(!AdmitAiNuclearOpponent(player, opponent, history));
    }
}

TEST_CASE("spy_unit_creation_failure_preserves_guest_state", "[ctest][gameplay-fixes]")
{
    PPCRegister returnedSlot{};
    for (const uint64_t result : { 0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull, 0x12345678FFFFFFFFull })
    {
        returnedSlot.u64 = result;
        CHECK(SkipFailedSpyUnitCreation(returnedSlot));
        CHECK(returnedSlot.u64 == result);
    }
    for (uint32_t slot = 0; slot < 256; ++slot)
    {
        const uint64_t result = 0x1234567800000000ull | slot;
        returnedSlot.u64      = result;
        CHECK(!SkipFailedSpyUnitCreation(returnedSlot));
        CHECK(returnedSlot.u64 == result);
    }
}

TEST_CASE("first_discovery_hooks_preserve_guest_state", "[ctest][gameplay-fixes]")
{
    PPCRegister building{};
    building.u64 = 0x1234567800000003ull;
    CHECK(AdmitFirstDiscoveryTempleCity(building));
    CHECK(building.u64 == 0x1234567800000003ull);
    building.s64 = 103;
    CHECK(!AdmitFirstDiscoveryTempleCity(building));

    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto* memory = runtime.memory();
    auto* heap   = memory->LookupHeap(0x830E0000);
    REQUIRE((heap && heap->AllocFixed(0x830E0000, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x830ECD28 + 2 * 4), 1);
    PPCRegister player{}, technology{}, source{}, phase{};
    player.u64     = 0x1234567800000002ull;
    technology.u64 = 0x123456780000000Bull;
    source.u64     = 0x1234567800000006ull;
    for (const uint32_t flag : { 0u, 1u })
    {
        rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x830E3438), flag);
        for (const uint32_t nativePhase : { 1u, 2u })
        {
            phase.u64 = 0x1234567800000000ull | nativePhase;
            CHECK(PreserveEgyptianIrrigationReward(player, technology, source, phase));
            CHECK(rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(0x830E3438)) == flag);
            CHECK(phase.u64 == (0x1234567800000000ull | nativePhase));
        }
    }
    CHECK(player.u64 == 0x1234567800000002ull);
    CHECK(technology.u64 == 0x123456780000000Bull);
    CHECK(source.u64 == 0x1234567800000006ull);
    source.s64 = 0;
    CHECK(!PreserveEgyptianIrrigationReward(player, technology, source, phase));
    source.s64 = 6;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x830ECD28 + 2 * 4), 2);
    CHECK(!PreserveEgyptianIrrigationReward(player, technology, source, phase));

    // City records are 188 bytes with the signed owner byte first.
    constexpr uint32_t cityTableAddress = 0x830E4000;
    auto               ownerByte        = [&](uint32_t city)
    {
        return memory->TranslateVirtual<uint8_t*>(cityTableAddress + city * 188);
    };
    *ownerByte(5)   = 3;
    *ownerByte(6)   = 0xFF;
    *ownerByte(127) = 3;
    PPCRegister savedCity{}, cityTable{}, acquirer{};
    cityTable.u64 = 0x1234567800000000ull | cityTableAddress;
    acquirer.u64  = 0x1234567800000003ull;
    savedCity.u64 = 0x1234567800000005ull;
    CHECK(!SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
    CHECK(savedCity.u64 == 0x1234567800000005ull);
    CHECK(cityTable.u64 == (0x1234567800000000ull | cityTableAddress));
    CHECK(acquirer.u64 == 0x1234567800000003ull);
    savedCity.s64 = 127;
    CHECK(!SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
    savedCity.s64 = 6;
    CHECK(SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
    savedCity.s64 = 5;
    acquirer.s64  = 4;
    CHECK(SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
    acquirer.s64 = 3;
    for (const int64_t invalid : { int64_t{ -1 }, int64_t{ 128 }, int64_t{ 0xFFFFFFFF } })
    {
        savedCity.s64 = invalid;
        CHECK(SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
    }
    savedCity.s64 = 5;
    cityTable.u64 = 0x1000;
    CHECK(SuppressStaleFirstDiscoveryCaption(savedCity, cityTable, acquirer));
}

TEST_CASE("first_discovery_caption_requires_acquirer_owned_city", "[ctest][gameplay-fixes]")
{
    CHECK(!gf::SuppressStaleFirstDiscoveryCaption(0, 0, 0));
    CHECK(!gf::SuppressStaleFirstDiscoveryCaption(127, 7, 7));
    CHECK(gf::SuppressStaleFirstDiscoveryCaption(5, -1, 3));
    CHECK(gf::SuppressStaleFirstDiscoveryCaption(5, -1, -1));
    CHECK(gf::SuppressStaleFirstDiscoveryCaption(5, 2, 3));
    CHECK(gf::SuppressStaleFirstDiscoveryCaption(-1, 3, 3));
    CHECK(gf::SuppressStaleFirstDiscoveryCaption(128, 3, 3));
}

TEST_CASE("literacy_courthouse_reward_suppressed", "[ctest][gameplay-fixes]")
{
    // Technology 12 (Literacy) with normalized building 14 (Courthouse) must be
    // reported as no match. Ceremonial Burial/Temple and other pairs stay native.
    CHECK(gf::SuppressLiteracyCourthouseReward(12, 14));
    CHECK(!gf::SuppressLiteracyCourthouseReward(3, 3));
    CHECK(!gf::SuppressLiteracyCourthouseReward(12, 3));
    CHECK(!gf::SuppressLiteracyCourthouseReward(3, 14));
    CHECK(!gf::SuppressLiteracyCourthouseReward(11, 14));
    CHECK(!gf::SuppressLiteracyCourthouseReward(12, 13));
    CHECK(!gf::SuppressLiteracyCourthouseReward(112, 14));
    CHECK(!gf::SuppressLiteracyCourthouseReward(12, 114));

    PPCRegister technology{}, building{};
    technology.u64 = 0x123456780000000Cull;
    building.u64   = 0x123456780000000Eull;
    CHECK(SuppressLiteracyCourthouseReward(technology, building));
    CHECK(technology.u64 == 0x123456780000000Cull);
    CHECK(building.u64 == 0x123456780000000Eull);
    building.s64 = 3;
    CHECK(!SuppressLiteracyCourthouseReward(technology, building));
    technology.s64 = 3;
    CHECK(!SuppressLiteracyCourthouseReward(technology, building));
    building.s64 = 14;
    CHECK(!SuppressLiteracyCourthouseReward(technology, building));
}

TEST_CASE("temple_first_discovery_population_threshold", "[ctest][gameplay-fixes]")
{
    CHECK(gf::BypassTemplePopulationThreshold(3));
    for (int32_t index = -1; index <= 23; ++index)
    {
        if (index != 3)
        {
            CHECK(!gf::BypassTemplePopulationThreshold(index));
        }
    }
    CHECK(!gf::BypassTemplePopulationThreshold(103));
}

TEST_CASE("egypt_free_irrigation_discovery_phases", "[ctest][gameplay-fixes]")
{
    CHECK(gf::PreserveEgyptianIrrigationDiscovery(11, 6, 1, 1));
    CHECK(gf::PreserveEgyptianIrrigationDiscovery(11, 6, 2, 1));
    for (const int32_t phase : { -1, 0, 3, 4 })
    {
        CHECK(!gf::PreserveEgyptianIrrigationDiscovery(11, 6, phase, 1));
    }
    CHECK(!gf::PreserveEgyptianIrrigationDiscovery(11, 0, 1, 1));
    CHECK(!gf::PreserveEgyptianIrrigationDiscovery(11, 5, 1, 1));
    CHECK(!gf::PreserveEgyptianIrrigationDiscovery(3, 6, 1, 1));
    CHECK(!gf::PreserveEgyptianIrrigationDiscovery(11, 6, 1, 0));
    CHECK(!gf::PreserveEgyptianIrrigationDiscovery(11, 6, 1, 2));
}

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
    turns = 7;
    for (const char* bad : { "", "0", "1", "2", "11", "-1", " 5", "5 ", "05", "x" })
    {
        UNSCOPED_INFO("text=" << bad);
        CHECK(!gf::ParseDominationMinimumTurns(bad, turns));
        CHECK(turns == 7);
    }
}

TEST_CASE("domination_minimum_turns_policy", "[ctest][gameplay-fixes]")
{
    CHECK(gf::DominationMinimumTurns("5", true) == 5);
    CHECK(gf::DominationMinimumTurns("10", true) == 10);
    CHECK(gf::DominationMinimumTurns("1", true) == 10);
    CHECK(gf::DominationMinimumTurns("5", false) == 10);
    CHECK(gf::DominationMinimumTurns("bogus", true) == 10);
}

TEST_CASE("domination_shortened_minimum_latch", "[ctest][gameplay-fixes]")
{
    CHECK(!gf::ShortenedMinimumAllowed());
    gf::NoteNewGame();
    CHECK(gf::ShortenedMinimumAllowed());
    gf::NoteGameLoad();
    CHECK(!gf::ShortenedMinimumAllowed());
}

TEST_CASE("turn_limit_choices_map_to_indexes", "[ctest][gameplay-fixes]")
{
    for (const auto& [text, index] : std::array<std::pair<const char*, int32_t>, 5>{ { { "2100", 0 }, { "2200", 1 }, { "2300", 2 }, { "2500", 3 }, { "unlimited", 4 } } })
    {
        UNSCOPED_INFO("text=" << text);
        CHECK(gf::ParseTurnLimit(text) == index);
        CHECK(gf::IsValidTurnLimit(index));
    }
    CHECK(!gf::IsValidTurnLimit(5));
    CHECK(!gf::ExtendsTurnLimit(gf::kTurnLimitNative));
    CHECK(gf::ExtendsTurnLimit(1));
    CHECK(gf::ExtendsTurnLimit(3));
    CHECK(!gf::ExtendsTurnLimit(gf::kTurnLimitUnlimited));
}

TEST_CASE("turn_limit_cap_turns_follow_the_calendar", "[ctest][gameplay-fixes]")
{
    // Year = 2 * turn + 1714 from turn 118; the native 2100 cap is turn 193.
    CHECK(gf::TurnLimitCapTurn(0) == 193);
    CHECK(gf::TurnLimitCapTurn(1) == 243);
    CHECK(gf::TurnLimitCapTurn(2) == 293);
    CHECK(gf::TurnLimitCapTurn(3) == 393);
}

TEST_CASE("turn_limit_warning_is_five_turns_before_the_deadline", "[ctest][gameplay-fixes]")
{
    for (const auto& [limit, warning] : std::array<std::pair<int32_t, int32_t>, 3>{ { { 1, 238 }, { 2, 288 }, { 3, 388 } } })
    {
        for (const int32_t start : { 0, 1, 40 })
        {
            CHECK(gf::TurnLimitDeadlineTurn(limit, -1, start) == gf::TurnLimitCapTurn(limit));
            for (int32_t turn = warning - 3; turn <= warning + 3; ++turn)
            {
                UNSCOPED_INFO("limit=" << limit << " turn=" << turn << " start=" << start);
                CHECK(gf::TurnLimitWarningDue(limit, -1, start, turn) == (turn == warning));
            }
        }
    }
}

TEST_CASE("turn_limit_scenario_deadline_waits_for_200_elapsed_turns", "[ctest][gameplay-fixes]")
{
    // The native deadline needs the year at the cap and, outside the sentinel scenario, 200 elapsed turns.
    for (const auto& [start, deadline] : std::array<std::pair<int32_t, int64_t>, 3>{ { { 44, 244 }, { 43, 243 }, { 42, 243 } } })
    {
        UNSCOPED_INFO("start=" << start);
        CHECK(gf::TurnLimitDeadlineTurn(1, 7, start) == deadline);
        int64_t first = 0;
        while (!(2 * first + 1714 >= 2200 && first - start >= 200))
        {
            ++first;
        }
        CHECK(first == deadline);
        CHECK(gf::TurnLimitWarningDue(1, 7, start, static_cast<int32_t>(deadline - 5)));
        CHECK(!gf::TurnLimitWarningDue(1, 7, start, static_cast<int32_t>(deadline - 4)));
    }
    CHECK(gf::TurnLimitDeadlineTurn(3, 7, 250) == 450);
    CHECK(gf::TurnLimitWarningDue(3, 7, 250, 445));
    CHECK(gf::TurnLimitDeadlineTurn(3, 7, 100) == 393);
}

TEST_CASE("turn_limit_year_test_uses_the_extended_cap", "[ctest][gameplay-fixes]")
{
    CHECK(gf::BeforeTurnLimitYear(1, 2198));
    CHECK(!gf::BeforeTurnLimitYear(1, 2200));
    CHECK(!gf::BeforeTurnLimitYear(1, 2202));
    CHECK(gf::BeforeTurnLimitYear(3, 2498));
    CHECK(!gf::BeforeTurnLimitYear(3, 2500));
}

TEST_CASE("turn_limit_hooks_pass_native_and_decide_extended_games", "[ctest][gameplay-fixes]")
{
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto* memory = runtime.memory();
    auto* heap   = memory->LookupHeap(0x83120000);
    REQUIRE((heap && heap->AllocFixed(0x83120000, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    const auto calendar = [&](int32_t scenario, int32_t start, int32_t turn)
    {
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8D4), scenario);
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8D8), start);
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8DC), turn);
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8E0), 2 * turn + 1714);
    };
    const auto restore = gf::TurnLimitForGame();

    gf::RestoreTurnLimit(gf::kTurnLimitNative);
    for (const int32_t turn : { 188, 192, 193, 194, 238, 243 })
    {
        calendar(-1, 1, turn);
        CHECK(!SkipUnlimitedTurnDeadline());
        CHECK(!SkipTurnLimitWarning());
        CHECK(!AdmitExtendedTurnLimitWarning());
        CHECK(!SkipExtendedTurnLimitDeadline());
    }

    gf::RestoreTurnLimit(gf::kTurnLimitUnlimited);
    calendar(-1, 1, 243);
    CHECK(SkipUnlimitedTurnDeadline());
    CHECK(!SkipTurnLimitWarning());
    CHECK(!AdmitExtendedTurnLimitWarning());
    CHECK(!SkipExtendedTurnLimitDeadline());

    gf::RestoreTurnLimit(1);
    CHECK(!SkipUnlimitedTurnDeadline());
    CHECK(AdmitExtendedTurnLimitWarning());
    for (const int32_t turn : { 188, 189, 237, 239, 242 })
    {
        calendar(-1, 1, turn);
        CHECK(SkipTurnLimitWarning());
    }
    calendar(-1, 1, 238);
    CHECK(!SkipTurnLimitWarning());
    for (const int32_t turn : { 193, 242 })
    {
        calendar(-1, 1, turn);
        CHECK(SkipExtendedTurnLimitDeadline());
    }
    for (const int32_t turn : { 243, 244, 400 })
    {
        calendar(-1, 1, turn);
        CHECK(!SkipExtendedTurnLimitDeadline());
    }
    calendar(5, 100, 295);
    CHECK(!SkipTurnLimitWarning());
    calendar(5, 100, 238);
    CHECK(SkipTurnLimitWarning());

    gf::RestoreTurnLimit(restore);
}

TEST_CASE("spaceship_tag_matches_only_its_actor_and_finishes_in_the_corridor", "[ctest][gameplay-fixes]")
{
    const int32_t tag = gf::SpaceshipActorIndex(2, 7);
    CHECK(tag == 519);
    CHECK(gf::MatchesSpaceshipTag(tag, 2, 7));
    CHECK(!gf::MatchesSpaceshipTag(tag, 2, 6));
    CHECK(!gf::MatchesSpaceshipTag(tag, 3, 7));

    CHECK(gf::FinishesSpaceshipEarly(1000));
    CHECK(!gf::FinishesSpaceshipEarly(1001));
}

TEST_CASE("starting_era_choices_map_to_native_values", "[ctest][gameplay-fixes]")
{
    for (const auto& [text, index] : std::array<std::pair<const char*, int32_t>, 5>{ { { "ancient", 0 }, { "medieval", 1 }, { "industrial", 2 }, { "modern", 3 }, { "future", 4 } } })
    {
        UNSCOPED_INFO("text=" << text);
        CHECK(gf::ParseStartingEra(text) == index);
        CHECK(gf::IsValidStartingEra(index));
    }
    CHECK(gf::StartingEraYear(4) == 2025);
    CHECK(!gf::IsValidStartingEra(5));
}

TEST_CASE("starting_era_applies_only_to_ordinary_games", "[ctest][gameplay-fixes]")
{
    CHECK(!gf::AppliesStartingEra(gf::kStartingEraNative, -1));
    for (const int32_t era : { 1, 2, 3, 4 })
    {
        CHECK(gf::AppliesStartingEra(era, -1));
        for (const int32_t scenario : { 0, 4, 7 })
        {
            CHECK(!gf::AppliesStartingEra(era, scenario));
        }
    }
}

TEST_CASE("starting_era_hook_writes_only_the_admitted_ordinary_halfword", "[ctest][gameplay-fixes]")
{
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto*              memory = runtime.memory();
    constexpr uint32_t kQ     = 0x830E8FD0;
    for (const uint32_t start : { 0x830E0000u, 0x83120000u })
    {
        auto* heap = memory->LookupHeap(start);
        REQUIRE((heap && heap->AllocFixed(start, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    }
    const auto restore = gf::StartingEraForGame();
    const auto field   = [&]
    {
        return rex::memory::load_and_swap<int16_t>(memory->TranslateVirtual(kQ + 0x1C));
    };
    const auto year = [&]
    {
        return rex::memory::load_and_swap<int16_t>(memory->TranslateVirtual(kQ + 0x3E));
    };
    const auto run = [&](bool admitted, int32_t era, int32_t scenario)
    {
        rex::memory::store_and_swap<int16_t>(memory->TranslateVirtual(kQ + 0x1C), 7);
        rex::memory::store_and_swap<int16_t>(memory->TranslateVirtual(kQ + 0x3E), 7);
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8D4), scenario);
        gf::RestoreStartingEra(era);
        rerevved::gameplay_session::SetupGate gate(admitted);
        PPCRegister                           r31;
        r31.u64 = kQ;
        ApplyStartingEra(r31);
        CHECK(r31.u64 == kQ);
    };

    for (const auto [era, start] : std::array<std::pair<int32_t, int16_t>, 4>{ { { 1, 1000 }, { 2, 1500 }, { 3, 1800 }, { 4, 2025 } } })
    {
        run(true, era, -1);
        CHECK(field() == era);
        CHECK(year() == start);
    }
    run(true, 0, -1);
    CHECK((field() == 7 && year() == 7));
    run(true, 3, 4);
    CHECK((field() == 7 && year() == 7));
    {
        rerevved::gameplay_session::SetupGate load(true, false);
        rex::memory::store_and_swap<int32_t>(memory->TranslateVirtual(0x8312B8D4), -1);
        gf::RestoreStartingEra(3);
        PPCRegister r31;
        r31.u64 = kQ;
        ApplyStartingEra(r31);
        CHECK(field() == 7);
    }
    gf::RestoreStartingEra(restore);
}

TEST_CASE("multiplayer_ai_difficulty_choices_map_to_native_values", "[ctest][gameplay-fixes]")
{
    for (const auto& [text, value] : std::array<std::pair<const char*, int32_t>, 3>{ { { "king", 2 }, { "emperor", 3 }, { "deity", 4 } } })
    {
        UNSCOPED_INFO("text=" << text);
        CHECK(gf::ParseMultiplayerAiDifficulty(text) == value);
        CHECK(gf::IsValidMultiplayerAiDifficulty(value));
    }
    CHECK(!gf::IsValidMultiplayerAiDifficulty(1));
    CHECK(!gf::IsValidMultiplayerAiDifficulty(5));
}

TEST_CASE("multiplayer_ai_difficulty_hook_replaces_the_literal_only_during_setup", "[ctest][gameplay-fixes]")
{
    const auto restore = gf::MultiplayerAiDifficultyForGame();
    const auto run     = [](bool admitted, bool newGame, int32_t difficulty)
    {
        gf::RestoreMultiplayerAiDifficulty(difficulty);
        rerevved::gameplay_session::SetupGate gate(admitted, newGame);
        PPCRegister                           r11;
        r11.u64 = 2;
        ApplyMultiplayerAiDifficulty(r11);
        return r11.s64;
    };
    for (const int32_t difficulty : { 2, 3, 4 })
    {
        CHECK(run(true, true, difficulty) == difficulty);
    }
    CHECK(run(true, false, 4) == 2);
    CHECK(run(false, true, 4) == 2);
    gf::RestoreMultiplayerAiDifficulty(restore);
}

TEST_CASE("domination_elapsed_rebase_matches_threshold", "[ctest][gameplay-fixes]")
{
    for (const int32_t minimum : { 5, 10 })
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
    CHECK(gf::RebaseDominationElapsed(INT32_MAX, 5) == INT32_MAX);
    CHECK(gf::RebaseDominationElapsed(INT32_MIN, 10) == INT32_MIN);
}

namespace
{

constexpr uint32_t kBonusLookup    = 0x82CF0CB0;
constexpr uint32_t kCityGoldIncome = 0x82D148C0;
constexpr uint32_t kUnitQuote      = 0x82CE28B0;
constexpr uint32_t kUnitConversion = 0x82D17974;
constexpr int32_t  kIndustrialEra  = 2;
constexpr int32_t  kModernEra      = 3;
constexpr int32_t  kMedievalEra    = 1;

// The lookup result a rate consumer sees once the filter has run.
bool filteredBonus(bool active, int32_t ability, uint64_t returnAddress, uint32_t baseline = 0)
{
    return active && !gf::SuppressBonusLookupResult(ability, 0, returnAddress, baseline);
}

// Native rush rate numerator: (1 + bonus25) * era + 2.
int32_t rushRate(bool bonus25, int32_t era)
{
    return (bonus25 ? 2 : 1) * era + 2;
}

// Native UEA8 threshold from the composed baseline B.
int32_t growthThreshold(bool active, int32_t baseline)
{
    return active ? std::max(20, baseline - 10) : baseline;
}

} // namespace

TEST_CASE("gold_bonus_filter_matches_only_rate_consumers", "[ctest][gameplay-fixes]")
{
    for (const uint32_t rateReturn : gf::kGoldBonusRateReturns)
    {
        UNSCOPED_INFO("return=" << std::hex << rateReturn);
        CHECK(gf::SuppressBonusLookupResult(25, 0, rateReturn, 0));
        CHECK(!gf::SuppressBonusLookupResult(25, 1, rateReturn, 0));
        CHECK(!gf::SuppressBonusLookupResult(24, 0, rateReturn, 0));
        CHECK(!gf::SuppressBonusLookupResult(8, 0, rateReturn, 0));
        CHECK(!gf::SuppressBonusLookupResult(25, 0, rateReturn - 4, 0));
    }
    // Native LR values, independent of the production table.
    const std::set<uint32_t> handoffReturns = {
        0x82CB3468,
        0x82CB67C0,
        0x82CB68E4,
        0x82CB69E8,
        0x82CB6BD0,
        0x82CB6CE0,
        0x82CB6DC4,
        0x82CE28B0,
        0x82CE2994,
        0x82CE2A4C,
        0x82CEC324,
        0x82CEC454,
        0x82CEC564,
        0x82D17974,
        0x82D17A08,
        0x82D17A50,
    };
    CHECK(std::set<uint32_t>(gf::kGoldBonusRateReturns.begin(), gf::kGoldBonusRateReturns.end()) == handoffReturns);
    for (const uint32_t preserved : { kCityGoldIncome, 0x82CB5B80u, 0x82CB5E1Cu, 0x82CED550u, gf::kGrowthThresholdReturn })
    {
        CHECK(!gf::SuppressBonusLookupResult(25, 0, preserved, 0));
    }
}

TEST_CASE("gold_bonus_rush_rates_follow_era", "[ctest][gameplay-fixes]")
{
    // Industrial, 10 remaining Production, ordinary unit or building.
    CHECK(rushRate(true, kIndustrialEra) * 10 == 60);
    CHECK(rushRate(filteredBonus(true, 25, kUnitQuote), kIndustrialEra) * 10 == 40);
    CHECK(rushRate(filteredBonus(true, 25, kUnitConversion), kIndustrialEra) * 10 == 40);
    CHECK(rushRate(filteredBonus(true, 25, kUnitQuote), kIndustrialEra - 1) == 3);
    // Industrial Wonder at twice the building rate.
    CHECK(2 * rushRate(true, kIndustrialEra) * 10 == 120);
    CHECK(2 * rushRate(filteredBonus(true, 25, 0x82CE2A4C), kIndustrialEra) * 10 == 80);
    // Modern SS Propulsion uses the building arm.
    CHECK(rushRate(true, kModernEra) * 189 == 1512);
    CHECK(rushRate(filteredBonus(true, 25, 0x82CE2994), kModernEra) * 189 == 945);
    // Medieval America with bonus35 and inactive bonus25.
    const int32_t americanRate = rushRate(filteredBonus(false, 25, kUnitQuote), kMedievalEra) / 2;
    CHECK(americanRate * 10 == 10);
    // City Gold income keeps G + trunc0(G / 2).
    const int32_t gold = 5;
    CHECK(gold + (filteredBonus(true, 25, kCityGoldIncome) ? gold / 2 : 0) == 7);
}

TEST_CASE("zulu_growth_filter_keeps_small_baselines", "[ctest][gameplay-fixes]")
{
    const std::vector<std::pair<int32_t, int32_t>> cases = {
        { 10, 10 },
        { 19, 19 },
        { 20, 20 },
        { 25, 20 },
        { 30, 20 },
        { 40, 30 },
    };
    for (const auto& [baseline, corrected] : cases)
    {
        UNSCOPED_INFO("baseline=" << baseline);
        const bool active = filteredBonus(true, 8, gf::kGrowthThresholdReturn, static_cast<uint32_t>(baseline));
        CHECK(growthThreshold(active, baseline) == corrected);
        CHECK(growthThreshold(false, baseline) == baseline);
    }
    for (uint32_t baseline = 0; baseline < 20; ++baseline)
    {
        CHECK(gf::SuppressBonusLookupResult(8, 0, gf::kGrowthThresholdReturn, baseline));
    }
    for (const int32_t negative : { -1, -10, INT32_MIN })
    {
        CHECK(!gf::SuppressBonusLookupResult(8, 0, gf::kGrowthThresholdReturn, static_cast<uint32_t>(negative)));
    }
    CHECK(!gf::SuppressBonusLookupResult(8, 1, gf::kGrowthThresholdReturn, 10));
    CHECK(!gf::SuppressBonusLookupResult(8, 0, kUnitQuote, 10));
    CHECK(!gf::SuppressBonusLookupResult(25, 0, gf::kGrowthThresholdReturn, 10));
}

TEST_CASE("bonus_lookup_hook_changes_only_its_result", "[ctest][gameplay-fixes]")
{
    namespace gi = rerevved::tests::guest_imports;

    struct Call
    {
        int32_t  ability;
        uint64_t returnAddress;
        uint64_t baseline;
        uint64_t nativeResult;
        uint64_t expected;
    };

    const std::vector<Call> calls = {
        { 25, kUnitQuote, 0x1234567800000003ull, 1, 0 },
        { 25, kCityGoldIncome, 0x1234567800000003ull, 1, 1 },
        { 25, kUnitQuote, 0x1234567800000003ull, 0, 0 },
        { 8, gf::kGrowthThresholdReturn, 0x123456780000000Aull, 1, 0 },
        { 8, gf::kGrowthThresholdReturn, 0x1234567800000019ull, 1, 1 },
        { 8, gf::kGrowthThresholdReturn, 0xFFFFFFFFFFFFFFF6ull, 1, 1 },
        { 7, kUnitQuote, 0x1234567800000003ull, 0x1234567800000001ull, 0x1234567800000001ull },
    };
    for (const Call& call : calls)
    {
        UNSCOPED_INFO("ability=" << call.ability << " return=" << std::hex << call.returnAddress);
        PPCContext ctx{};
        ctx.r3.u64              = 0xABCD000000000000ull | static_cast<uint32_t>(call.ability);
        ctx.r4.u64              = 0x1234567800000002ull;
        ctx.r5.u64              = 0x1234567800000000ull;
        ctx.r31.u64             = call.baseline;
        ctx.lr                  = call.returnAddress;
        const uint64_t   before = gi::ImportCallCount(kBonusLookup);
        gi::ScopedImport lookup(kBonusLookup, [&](uint32_t, PPCContext& guest, uint8_t*)
                                {
                                    CHECK(guest.r3.s32 == call.ability);
                                    CHECK(guest.r5.u64 == 0x1234567800000000ull);
                                    CHECK(guest.lr == call.returnAddress);
                                    guest.r3.u64  = call.nativeResult;
                                    guest.r4.u64  = 0x00000000000000AAull;
                                    guest.r9.u64  = 0x00000000000000BBull;
                                    guest.r31.u64 = 0x00000000000000CCull;
                                });
        sub_82CF0CB0(ctx, nullptr);
        CHECK(gi::ImportCallCount(kBonusLookup) == before + 1);
        CHECK(ctx.r3.u64 == call.expected);
        CHECK(ctx.r4.u64 == 0xAAull);
        CHECK(ctx.r9.u64 == 0xBBull);
        CHECK(ctx.r31.u64 == 0xCCull);
        CHECK(ctx.lr == call.returnAddress);
    }
}

TEST_CASE("always_at_war_projects_only_pairs_of_the_mask", "[ctest][gameplay-fixes]")
{
    std::array<uint32_t, 64> relation{};
    relation.fill(0xFFFFFFFFu);
    relation[8 * 0 + 1] = 1;
    relation[8 * 2 + 0] = 1;
    relation[8 * 1 + 1] = 7;
    relation[8 * 1 + 3] = 1;
    relation[8 * 5 + 0] = 1;
    const auto native   = relation;

    CHECK(gf::ProjectWar(relation, 0x07) == 6);
    for (int32_t p = 0; p < 8; ++p)
    {
        for (int32_t q = 0; q < 8; ++q)
        {
            const auto cell = static_cast<size_t>(8 * p + q);
            CHECK(relation[cell] == (p != q && p < 3 && q < 3 ? 0u : native[cell]));
        }
    }
    CHECK(gf::ProjectWar(relation, 0x07) == 0);
    CHECK(gf::ProjectWar(relation, 0x01) == 0);
}

TEST_CASE("always_at_war_civilizations_follow_setup_and_play", "[ctest][gameplay-fixes]")
{
    CHECK(gf::IntendedCivilizations(6, 5, 0x1, 4) == 0x1F);
    CHECK(gf::IntendedCivilizations(6, 5, 0x1, 2) == 0x07);
    CHECK(gf::IntendedCivilizations(6, 4, 0x1, 4) == 0);

    CHECK(gf::LiveCivilizations(6, 5, 0x24, 0x02) == 0x06);
    CHECK(gf::LiveCivilizations(6, 5, 0x20, 0x20) == 0);
    CHECK(gf::LiveCivilizations(6, 4, 0x0F, 0x0F) == 0);
}

TEST_CASE("always_at_war_forbids_only_peace_between_live_civilizations", "[ctest][gameplay-fixes]")
{
    CHECK(gf::ForbidsPeace(true, 0x07, 0, 2));
    CHECK(gf::ForbidsPeace(true, 0x07, 2, 0));
    CHECK_FALSE(gf::ForbidsPeace(false, 0x07, 0, 2));
    CHECK_FALSE(gf::ForbidsPeace(true, 0x07, 1, 1));
    CHECK_FALSE(gf::ForbidsPeace(true, 0x07, 0, 3));
    CHECK_FALSE(gf::ForbidsPeace(true, 0x07, 0, 8));
    CHECK_FALSE(gf::ForbidsPeace(true, 0x07, -1, 0));

    CHECK(gf::ForbidsRelationWrite(true, 0x07, 0, 1, 0, 1));
    CHECK(gf::ForbidsRelationWrite(true, 0x07, 0, 1, 0, 0xFFFFFFFFu));
    CHECK_FALSE(gf::ForbidsRelationWrite(true, 0x07, 0, 1, 0, 0));
    CHECK_FALSE(gf::ForbidsRelationWrite(true, 0x07, 0, 1, 1, 1));
    CHECK_FALSE(gf::ForbidsRelationWrite(false, 0x07, 0, 1, 0, 1));
    CHECK_FALSE(gf::ForbidsRelationWrite(true, 0x07, 0, 3, 0, 1));
}

TEST_CASE("always_at_war_hooks_project_and_refuse_by_live_pair", "[ctest][gameplay-fixes]")
{
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto* memory = runtime.memory();
    for (const auto& [page, size] : { std::pair<uint32_t, uint32_t>{ 0x82F70000u, 0x10000u }, { 0x830E0000u, 0x80000u } })
    {
        auto* heap = memory->LookupHeap(page);
        REQUIRE(heap);
        REQUIRE(heap->AllocFixed(page, size, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    }
    const auto store = [&](uint32_t address, uint32_t value)
    {
        rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(address), value);
    };
    const auto cell = [&](uint32_t p, uint32_t q)
    {
        return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(gf::kRelationMatrix + 4 * (8 * p + q)));
    };
    const auto resetMatrix = [&]
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            store(gf::kRelationMatrix + 4 * i, 0xFFFFFFFFu);
        }
    };
    store(0x82F700AC, 6);
    store(0x82F700B0, 5);
    store(0x8312E608, 0x1);
    resetMatrix();
    // Zeroed units read as live; free them first.
    for (uint32_t player = 0; player < 8; ++player)
    {
        for (uint32_t unit = 0; unit < 256; ++unit)
        {
            *memory->TranslateVirtual(0x830F2BF0 + player * 256 * 0x54 + unit * 0x54) = 0xFF;
        }
    }
    for (const uint32_t player : { 0u, 1u, 5u })
    {
        *memory->TranslateVirtual(0x830F2BF0 + player * 256 * 0x54) = static_cast<uint8_t>(player);
    }
    for (uint32_t city = 0; city < 128; ++city)
    {
        *memory->TranslateVirtual(0x8314FE28 + 188 * city) = 0xFF;
    }
    *memory->TranslateVirtual(0x8314FE28 + 188 * 4)     = 2;
    *memory->TranslateVirtual(0x8314FE28 + 188 * 4 + 3) = 3;
    *memory->TranslateVirtual(0x8314FE28 + 188 * 6)     = 3;

    const bool  saved = gf::AlwaysAtWarForGame();
    PPCRegister p{}, q{}, value{}, choice{};
    p.u64     = 0;
    q.u64     = 1;
    value.u64 = 1;

    gf::RestoreAlwaysAtWar(false);
    ProjectAlwaysAtWarOnLoad();
    CHECK(cell(0, 1) == 0xFFFFFFFFu);
    CHECK_FALSE(RefuseRelationRequest(p, q, value));
    choice.s64 = 2;
    RefuseContactChoice(choice, p, q);
    CHECK(choice.s64 == 2);

    gf::RestoreAlwaysAtWar(true);
    ProjectAlwaysAtWarOnLoad();
    for (uint32_t a = 0; a < 8; ++a)
    {
        for (uint32_t b = 0; b < 8; ++b)
        {
            CHECK(cell(a, b) == (a != b && a < 3 && b < 3 ? 0u : 0xFFFFFFFFu));
        }
    }

    CHECK(RefuseRelationRequest(p, q, value));
    CHECK(RefuseRelationRequest(q, p, value));
    CHECK(RefuseDeliveredRelation(p, q, value));
    value.s64 = -1;
    CHECK(RefuseRelationRequest(p, q, value));
    value.u64 = 0;
    CHECK_FALSE(RefuseRelationRequest(p, q, value));
    value.u64 = 1;
    for (const uint64_t other : { 3ull, 5ull })
    {
        q.u64 = other;
        CHECK_FALSE(RefuseRelationRequest(p, q, value));
    }

    q.u64      = 1;
    choice.s64 = 2;
    RefuseContactChoice(choice, p, q);
    CHECK(choice.s32 == -1);
    q.u64      = 3;
    choice.s64 = 2;
    RefuseContactChoice(choice, p, q);
    CHECK(choice.s64 == 2);

    resetMatrix();
    ProjectAlwaysAtWarEarly();
    ProjectAlwaysAtWarGroups();
    CHECK(cell(0, 1) == 0xFFFFFFFFu);
    {
        rerevved::gameplay_session::SetupGate gate(true);
        const int32_t                         expanded = rerevved::ai_civilizations::ExpandedAiLimit();
        const uint32_t                        intended = gf::IntendedCivilizations(6, 5, 0x1, expanded != 0 ? expanded : rex::cvar::Query<int32_t>("ai_civilizations"));
        ProjectAlwaysAtWarEarly();
        for (uint32_t a = 0; a < 8; ++a)
        {
            for (uint32_t b = 0; b < 8; ++b)
            {
                CHECK(cell(a, b) == (a != b && (intended >> a & 1) && (intended >> b & 1) ? 0u : 0xFFFFFFFFu));
            }
        }
    }
    gf::RestoreAlwaysAtWar(saved);
}
