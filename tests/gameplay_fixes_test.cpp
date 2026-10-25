#include "gameplay_fixes.h"
#include "guest_import_doubles.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

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
REX_EXTERN(sub_82CF0CB0);

namespace gf = rerevved::gameplay_fixes;

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
    // reported as no match; Ceremonial Burial/Temple and other pairs stay native.
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
    // The handoff's native LR column, independent of the production table.
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
