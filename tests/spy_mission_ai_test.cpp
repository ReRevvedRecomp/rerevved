#include "logging_scope.hpp"
#include "spy_mission_ai.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <spdlog/sinks/ostream_sink.h>

void ChooseAiSpyMission(PPCRegister& actor, PPCRegister& unit, PPCRegister& selector);

namespace
{

namespace spy = rerevved::spy_mission_ai;

constexpr uint32_t kTurn      = 0x8312B8DC;
constexpr uint32_t kCities    = 0x8314FE28;
constexpr uint32_t kGold      = 0x830ED544;
constexpr uint32_t kStatistic = 0x830E5AD0;

constexpr uint32_t kBanks         = 0x830F2BF0;
constexpr uint32_t kRng           = 0x82F7AD00;
constexpr uint32_t kClaims        = 0x830E5148 + 0xB8;
constexpr uint32_t kGeneralGate   = 0x8314F1C8;
constexpr uint32_t kPresencePlane = 0x8312A608;
constexpr uint32_t kProtectGroups = 0x830E9070;

// Banks of players 1..3 span 0x830F83F0..0x831083F0.
constexpr std::array<uint32_t, 7> kPages{ 0x82F70000, 0x830E0000, 0x830F0000, 0x83100000, 0x83120000, 0x83140000, 0x83150000 };

// Target city 5 is owned by player 2. The acting player is 1, the Spy slot 7.
constexpr uint32_t kCity  = 5;
constexpr uint32_t kOwner = 2;

constexpr uint64_t kActorBits    = 0x1234567800000001ull;
constexpr uint64_t kUnitBits     = 0x9ABCDEF000000007ull;
constexpr uint32_t kCityRecord   = kCities + 0xBC * kCity;
constexpr uint32_t kBuildingWord = kCityRecord + 0x10;
constexpr uint32_t kItemHalf     = kCityRecord + 0x38;

// The target city's raw coordinates, where the victims stand.
constexpr uint32_t kCityX = 10;
constexpr uint32_t kCityY = 12;

struct World
{
    rerevved::tests::ScopedLogging logging{ false };
    rex::Runtime                   runtime{ std::filesystem::path{} };

    World()
    {
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const uint32_t page : kPages)
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        Byte(kCityRecord + 0, kOwner);
        Production(false);
        Turn(1);
        Gold(kOwner, 100, 1);
    }

    uint8_t* Bytes(uint32_t address)
    {
        return runtime.memory()->TranslateVirtual<uint8_t*>(address);
    }

    void Byte(uint32_t address, uint32_t value)
    {
        *Bytes(address) = static_cast<uint8_t>(value);
    }

    void Word(uint32_t address, uint32_t value)
    {
        rex::memory::store_and_swap<uint32_t>(Bytes(address), value);
    }

    void Turn(uint32_t turn)
    {
        Word(kTurn, turn);
    }

    // Player reserve and city-count statistic of the target owner.
    void Gold(uint32_t owner, uint32_t reserve, uint32_t statistic)
    {
        Word(kGold + 4 * owner, reserve);
        Word(kStatistic + 0x80 * owner, statistic);
    }

    void Production(bool present)
    {
        Byte(kItemHalf, present ? 0x00 : 0xFF);
        Byte(kItemHalf + 1, present ? 0x64 : 0xFF);
    }

    void Buildings(uint32_t bits)
    {
        Word(kBuildingWord, bits);
    }

    // Population 3, reserve 100, one city: amount 100 on a reserve of 100.
    void StealableGold()
    {
        Byte(kCityRecord + 3, 3);
        Gold(kOwner, 100, 1);
    }

    void Half(uint32_t address, uint32_t value)
    {
        Byte(address, value >> 8);
        Byte(address + 1, value & 0xFF);
    }

    // The RNG record's modulus and normalization double.
    void Rng(uint32_t modulus = spy::kRngModulus, uint64_t normalization = spy::kRngNormalization)
    {
        Word(kRng + 8, modulus);
        Word(kRng + 16, static_cast<uint32_t>(normalization >> 32));
        Word(kRng + 20, static_cast<uint32_t>(normalization));
    }

    void CityPosition(uint32_t x = kCityX, uint32_t y = kCityY)
    {
        Half(kCityRecord + 0x30, x);
        Half(kCityRecord + 0x32, y);
    }

    // The kidnap menu admits a city with a Great Person flag when Scotland Yard
    // is unclaimed (claim word -1).
    void OpenKidnapGate()
    {
        Half(kCityRecord + 0x3C, 1);
        Word(kClaims, 0xFFFFFFFF);
    }

    // The bribe menu needs the General halfword and a presence word at the city.
    void OpenBribeGate(uint32_t x = kCityX, uint32_t y = kCityY)
    {
        Half(kGeneralGate, 1);
        Half(kPresencePlane + 2 * (40 * x + y), 1);
    }

    // A record in the bank of `bank`, slot `slot`.
    void Unit(uint32_t bank, uint32_t slot, uint32_t owner, uint32_t type, uint32_t flags, uint32_t x = kCityX, uint32_t y = kCityY)
    {
        const uint32_t address = kBanks + 0x5400 * bank + 0x54 * slot;
        Byte(address, owner);
        Byte(address + 1, type);
        Word(address + 0xC, flags);
        Half(address + 0x1C, x);
        Half(address + 0x1E, y);
    }

    std::vector<uint8_t> Snapshot()
    {
        std::vector<uint8_t> bytes;
        for (const uint32_t page : kPages)
        {
            bytes.insert(bytes.end(), Bytes(page), Bytes(page) + 0x10000);
        }
        return bytes;
    }
};

struct Registers
{
    PPCRegister actor{};
    PPCRegister unit{};
    PPCRegister selector{};

    explicit Registers(uint64_t selectorBits = kCity)
    {
        actor.u64    = kActorBits;
        unit.u64     = kUnitBits;
        selector.u64 = selectorBits;
    }

    void Run()
    {
        ChooseAiSpyMission(actor, unit, selector);
    }
};

// The mission the effect reads from r5: the signed shift of its low word.
int32_t missionOf(const PPCRegister& selector)
{
    return selector.s32 >> 8;
}

} // namespace

TEST_CASE("spy_mission_ai_eligibility_follows_native_menu_predicates", "[ctest][spy-mission-ai]")
{
    spy::CityFacts city;
    CHECK(spy::EligibleMissions(city) == 0x01);

    city.currentItem = 0;
    CHECK(spy::EligibleMissions(city) == 0x03);
    city.currentItem = 307;
    CHECK(spy::EligibleMissions(city) == 0x03);
    city.currentItem = -1;

    // Bits 1..22 of the building word demolish; bit 0 and bits 23..31 do not.
    for (const uint32_t buildings : { 0x00000001u, 0x00800000u, 0xFF800001u })
    {
        city.buildings = buildings;
        CHECK(spy::EligibleMissions(city) == 0x01);
    }
    for (const uint32_t buildings : { 0x00000002u, 0x00400000u, 0xFFFFFFFFu })
    {
        city.buildings = buildings;
        CHECK(spy::EligibleMissions(city) == 0x09);
    }
    city.buildings = 0;

    // Gold: amount = trunc(S32(pop * reserve) / S32(statistic + 2)), above 1 and
    // not above the reserve. Unread inputs drop the mission.
    city.population = 3;
    city.reserve    = 100;
    city.statistic  = 1;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.goldKnown = true;
    CHECK(spy::EligibleMissions(city) == 0x21);
    city.population = 2;
    CHECK(spy::EligibleMissions(city) == 0x21);
    // 4 * 100 / 3 = 133 would overdraw the victim.
    city.population = 4;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.population = 1;
    city.reserve    = 3;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.reserve = 6;
    CHECK(spy::EligibleMissions(city) == 0x21);
    city.population = 10;
    city.reserve    = 100;
    city.statistic  = 5;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.statistic = 8;
    CHECK(spy::EligibleMissions(city) == 0x21);

    // Native divide traps, an empty reserve and negative amounts are not eligible.
    city.population = 2;
    city.reserve    = 0x40000000;
    city.statistic  = -3;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.statistic = -2;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.population = 3;
    city.reserve    = 0;
    city.statistic  = 1;
    CHECK(spy::EligibleMissions(city) == 0x01);
    city.population = -1;
    city.reserve    = 100;
    CHECK(spy::EligibleMissions(city) == 0x01);

    city.currentItem = 0;
    city.buildings   = 0x2;
    city.population  = 3;
    city.reserve     = 100;
    city.statistic   = 1;
    CHECK(spy::EligibleMissions(city) == 0x2B);
    CHECK(spy::DescribeMissions(0x2B) == "0,1,3,5");
    CHECK(spy::DescribeMissions(0x01) == "0");
}

TEST_CASE("spy_mission_ai_steal_gold_amount_matches_native_arithmetic", "[ctest][spy-mission-ai]")
{
    int32_t amount = 0;
    CHECK((spy::StealGoldAmount(3, 100, 1, amount) && amount == 100));
    CHECK((spy::StealGoldAmount(10, 100, 5, amount) && amount == 142));
    CHECK((spy::StealGoldAmount(-1, 100, 1, amount) && amount == -33));
    // pop * reserve wraps to INT32_MIN; the divisor is statistic + 2.
    CHECK((spy::StealGoldAmount(2, 0x40000000, 0, amount) && amount == -1073741824));
    CHECK(!spy::StealGoldAmount(2, 0x40000000, -3, amount));
    CHECK(!spy::StealGoldAmount(3, 100, -2, amount));
    CHECK((spy::StealGoldAmount(3, 100, -1, amount) && amount == 300));
}

TEST_CASE("spy_mission_ai_choice_is_a_pinned_uniform_pick", "[ctest][spy-mission-ai]")
{
    CHECK(spy::ChoiceHash(1, 1, 5, 7) == 0x41B381A9);
    CHECK(spy::ChoiceHash(3, 1, 5, 7) == 0xB996BF0F);
    CHECK(spy::ChoiceHash(4, 1, 5, 7) == 0x15CD9FBF);
    CHECK(spy::ChoiceHash(7, 1, 5, 7) == 0xDA1332D6);
    CHECK(spy::ChoiceHash(100, 0, 3, 0) == 0x96364348);
    CHECK(spy::ChoiceHash(100, 5, 127, 255) == 0x79957FA2);
    CHECK(spy::ChoiceHash(250, 3, 0, 19) == 0xD5533E84);
    CHECK(spy::ChoiceHash(1, 1, 5, 7) != spy::ChoiceHash(1, 1, 5, 8));
    CHECK(spy::ChoiceHash(1, 1, 5, 7) != spy::ChoiceHash(1, 1, 6, 7));
    CHECK(spy::ChoiceHash(1, 1, 5, 7) != spy::ChoiceHash(1, 2, 5, 7));

    // Only mission 0 eligible: every hash picks it.
    for (const uint32_t hash : { 0u, 0x41B381A9u, 0xFFFFFFFFu })
    {
        CHECK(spy::ChooseMission(0x01, hash) == 0);
    }
    // All four eligible: the hash range splits into four equal quarters.
    const std::array<std::pair<uint32_t, int32_t>, 8> quarters{ { { 0x00000000, 0 },
                                                                  { 0x3FFFFFFF, 0 },
                                                                  { 0x40000000, 1 },
                                                                  { 0x7FFFFFFF, 1 },
                                                                  { 0x80000000, 3 },
                                                                  { 0xBFFFFFFF, 3 },
                                                                  { 0xC0000000, 5 },
                                                                  { 0xFFFFFFFF, 5 } } };
    for (const auto& [hash, mission] : quarters)
    {
        UNSCOPED_INFO("hash=" << std::hex << hash);
        CHECK(spy::ChooseMission(0x2B, hash) == mission);
    }
    CHECK(spy::ChooseMission(0x2B, 0x41B381A9) == 1);
    CHECK(spy::ChooseMission(0x2B, 0xB996BF0F) == 3);
    CHECK(spy::ChooseMission(0x2B, 0x15CD9FBF) == 0);
    CHECK(spy::ChooseMission(0x2B, 0xDA1332D6) == 5);
    CHECK(spy::ChooseMission(0x03, 0xB996BF0F) == 1);
    CHECK(spy::ChooseMission(0x09, 0xB996BF0F) == 3);
    CHECK(spy::ChooseMission(0x21, 0xB996BF0F) == 5);

    // Turns 1..400 spread over the four missions with these exact counts.
    std::map<int32_t, int32_t> counts;
    for (uint32_t turn = 1; turn <= 400; ++turn)
    {
        ++counts[spy::ChooseMission(0x2B, spy::ChoiceHash(turn, 1, 5, 7))];
    }
    CHECK((counts == std::map<int32_t, int32_t>{ { 0, 115 }, { 1, 87 }, { 3, 99 }, { 5, 99 } }));
    CHECK(spy::PackSelector(5, 127) == 0x57F);
    CHECK(spy::PackSelector(0, 5) == 5);
}

TEST_CASE("spy_mission_ai_hook_chooses_among_all_eligible_missions", "[ctest][spy-mission-ai]")
{
    World world;
    world.Production(true);
    world.Buildings(0x2);
    world.StealableGold();

    // Turn -> mission for actor 1, city 5, Spy slot 7 with Disrupt, Sabotage,
    // Demolish and Gold; no victims and a zeroed RNG record keep 4 and 6 out.
    const std::array<std::pair<uint32_t, int32_t>, 6> expected{ { { 1, 1 }, { 3, 3 }, { 4, 0 }, { 5, 1 }, { 7, 5 }, { 12, 0 } } };
    for (const auto& [turn, mission] : expected)
    {
        UNSCOPED_INFO("turn=" << turn);
        world.Turn(turn);
        const auto before = world.Snapshot();
        Registers  registers;
        registers.Run();
        CHECK(registers.selector.s64 == spy::PackSelector(mission, kCity));
        CHECK(missionOf(registers.selector) == mission);
        CHECK(registers.selector.u8 == kCity);
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
        CHECK((world.Snapshot() == before));
    }
}

TEST_CASE("spy_mission_ai_hook_limits_the_pick_to_the_eligible_set", "[ctest][spy-mission-ai]")
{
    World world;

    // Only Disrupt: nothing changes for any turn.
    for (uint32_t turn = 1; turn <= 12; ++turn)
    {
        world.Turn(turn);
        Registers registers;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
    }

    struct Case
    {
        uint32_t turn;
        int32_t  mission;
    };

    const auto run = [&](const std::vector<Case>& cases)
    {
        for (const auto& [turn, mission] : cases)
        {
            UNSCOPED_INFO("turn=" << turn);
            world.Turn(turn);
            Registers registers;
            registers.Run();
            CHECK(registers.selector.s64 == spy::PackSelector(mission, kCity));
            CHECK(registers.actor.u64 == kActorBits);
            CHECK(registers.unit.u64 == kUnitBits);
        }
    };

    world.Production(true);
    run({ { 1, 0 }, { 3, 1 }, { 4, 0 }, { 7, 1 } });
    world.Production(false);

    world.Buildings(0x2);
    run({ { 1, 0 }, { 3, 3 }, { 7, 3 } });
    // Bit 23 alone is a menu entry the effect cannot execute.
    world.Buildings(0x00800000);
    run({ { 3, 0 }, { 7, 0 } });
    world.Buildings(0);

    world.StealableGold();
    run({ { 1, 0 }, { 3, 5 }, { 7, 5 } });
    // An amount above the victim's reserve (4 * 100 / 3), or of 1, is not offered.
    world.Byte(kCityRecord + 3, 4);
    run({ { 3, 0 }, { 7, 0 } });
    world.Byte(kCityRecord + 3, 1);
    world.Gold(kOwner, 3, 1);
    run({ { 3, 0 }, { 7, 0 } });
}

TEST_CASE("spy_mission_ai_hook_keeps_unusable_calls_native", "[ctest][spy-mission-ai]")
{
    World world;
    world.Production(true);
    world.Buildings(0x2);
    world.StealableGold();
    world.Turn(3);

    Registers chosen;
    chosen.Run();
    REQUIRE(chosen.selector.s64 == spy::PackSelector(3, kCity));

    const auto before = world.Snapshot();
    // No candidate city, an out-of-range city, actor or slot.
    for (const uint64_t selector : { 0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull, 128ull, 0x105ull })
    {
        Registers registers(selector);
        registers.Run();
        CHECK(registers.selector.u64 == selector);
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
    }
    for (const uint64_t actor : { 8ull, 0xFFFFFFFFull })
    {
        Registers registers;
        registers.actor.u64 = actor;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
        CHECK(registers.actor.u64 == actor);
    }
    {
        Registers registers;
        registers.unit.u64 = 256;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
    }
    // A city without an owner, or the actor's own city.
    world.Byte(kCityRecord, 0xFF);
    {
        Registers registers;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
    }
    world.Byte(kCityRecord, 1);
    {
        Registers registers;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
    }
    world.Byte(kCityRecord, 8);
    {
        Registers registers;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
    }
    world.Byte(kCityRecord, kOwner);
    CHECK((world.Snapshot() == before));
}

TEST_CASE("spy_mission_ai_hook_logs_each_real_choice", "[ctest][spy-mission-ai]")
{
    World              world;
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%l %v");
    rex::AddSink(sink);

    struct RemoveSink
    {
        spdlog::sink_ptr sink;

        ~RemoveSink()
        {
            rex::RemoveSink(sink);
        }
    } removeSink{ sink };

    const auto lines = [&]
    {
        rex::FlushLogging();
        return output.str();
    };

    // Only Disrupt is eligible: no choice, no line.
    {
        Registers registers;
        registers.Run();
    }
    CHECK(lines().find("GameplayFix: spy mission") == std::string::npos);

    world.Production(true);
    world.Buildings(0x2);
    world.StealableGold();
    world.Turn(3);
    {
        Registers registers;
        registers.Run();
    }
    CHECK(lines().find("info GameplayFix: spy mission actor=1 city=5 owner=2 unit=7 turn=3 eligible=0,1,3,5 chosen=3") != std::string::npos);

    // A hash that picks Disrupt keeps the native selector and still reports it.
    world.Turn(4);
    {
        Registers registers;
        registers.Run();
        CHECK(registers.selector.u64 == kCity);
    }
    CHECK(lines().find("info GameplayFix: spy mission actor=1 city=5 owner=2 unit=7 turn=4 eligible=0,1,3,5 chosen=0") != std::string::npos);
}

TEST_CASE("spy_mission_ai_victims_follow_the_native_loop_acceptance", "[ctest][spy-mission-ai]")
{
    spy::UnitRecord great{ 2, 33, kCityX, kCityY, 0x4 };
    CHECK(spy::KidnapVictim(great, kCityX, kCityY));
    for (const int32_t type : { 31, 34, 37 })
    {
        great.type = type;
        CHECK(spy::KidnapVictim(great, kCityX, kCityY));
    }
    // The native loop admits signed types 31..127; only 31..37 have creators.
    for (const int32_t type : { -1, 0, 30, 38, 127 })
    {
        great.type = type;
        CHECK(!spy::KidnapVictim(great, kCityX, kCityY));
    }
    great.type = 33;
    CHECK(!spy::KidnapVictim(great, kCityX + 1, kCityY));
    CHECK(!spy::KidnapVictim(great, kCityX, kCityY + 1));
    great.owner = 0xFF;
    CHECK(!spy::KidnapVictim(great, kCityX, kCityY));
    great.owner = 2;
    for (const uint32_t flags : { 0x0u, 0x80000004u, 0x3u })
    {
        great.flags = flags;
        CHECK(!spy::KidnapVictim(great, kCityX, kCityY));
    }
    great.flags = 0xFFFFFFFB;
    CHECK(!spy::KidnapVictim(great, kCityX, kCityY));
    great.flags = 0x7FFFFFFF;
    CHECK(spy::KidnapVictim(great, kCityX, kCityY));

    spy::UnitRecord general{ 2, 30, kCityX, kCityY, 0 };
    CHECK(spy::BribeVictim(general, kCityX, kCityY));
    general.flags = 0x7FFFFFFF;
    CHECK(spy::BribeVictim(general, kCityX, kCityY));
    for (const int32_t type : { -1, 0, 29, 31 })
    {
        general.type = type;
        CHECK(!spy::BribeVictim(general, kCityX, kCityY));
    }
    general.type = 30;
    CHECK(!spy::BribeVictim(general, kCityX, kCityY + 1));
    CHECK(!spy::BribeVictim(general, kCityX + 1, kCityY));
    general.owner = 0xFF;
    CHECK(!spy::BribeVictim(general, kCityX, kCityY));
    general.owner = 2;
    general.flags = 0x80000000;
    CHECK(!spy::BribeVictim(general, kCityX, kCityY));
    CHECK(!spy::BribeVictim({}, kCityX, kCityY));
    CHECK(!spy::KidnapVictim({}, kCityX, kCityY));
}

TEST_CASE("spy_mission_ai_menu_gates_match_the_native_predicates", "[ctest][spy-mission-ai]")
{
    spy::KidnapGate gate;
    gate.owner      = 2;
    gate.claimOwner = 0;
    CHECK(!spy::KidnapMenuGate(gate));

    // A city without the Great Person halfword is never offered.
    gate.claimOwner = -1;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.cityGreatPersons = 1;
    CHECK(spy::KidnapMenuGate(gate));
    gate.claimOwner = 0;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.network = true;
    CHECK(spy::KidnapMenuGate(gate));
    gate.network = false;

    // A: mode 0 and a nonzero relation word at the item.
    gate.item = 3;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.relation[3] = 0x10;
    CHECK(spy::KidnapMenuGate(gate));
    gate.mode = 2;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.mode = 0;
    for (const int32_t item : { -1, 8, 100 })
    {
        gate.item = item;
        CHECK(!spy::KidnapMenuGate(gate));
    }
    gate.item        = 3;
    gate.relation[3] = 0;

    // B: mode 1 and a differing-group player k below the count whose bit is set.
    gate.mode        = 1;
    gate.count       = 2;
    gate.relation[3] = 0x2;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.group[1] = 5;
    CHECK(spy::KidnapMenuGate(gate));
    gate.count = 1;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.count = 9;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.count = 0;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.mode        = 0;
    gate.relation[3] = 0;
    gate.group[1]    = 0;

    // D: a claim owner in another group.
    gate.claimOwner = 1;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.group[1] = 4;
    CHECK(spy::KidnapMenuGate(gate));
    gate.owner = 1;
    CHECK(!spy::KidnapMenuGate(gate));
    gate.owner      = 2;
    gate.claimOwner = 9;
    CHECK(!spy::KidnapMenuGate(gate));

    CHECK(spy::BribeMenuGate(1, 10, 12, 1));
    CHECK(!spy::BribeMenuGate(0, 10, 12, 1));
    CHECK(!spy::BribeMenuGate(1, 10, 12, 0));
    CHECK(!spy::BribeMenuGate(1, 40, 12, 1));
    CHECK(!spy::BribeMenuGate(1, 10, -1, 1));

    CHECK(spy::RngReachesEverySlot(0xFFFFCFEF, 0x3DF0000301109066ull));
    CHECK(!spy::RngReachesEverySlot(0xFFFFCFEE, 0x3DF0000301109066ull));
    CHECK(!spy::RngReachesEverySlot(0, 0x3DF0000301109066ull));
    CHECK(!spy::RngReachesEverySlot(0xFFFFCFEF, 0x3DF0000301109067ull));
    CHECK(!spy::RngReachesEverySlot(0xFFFFCFEF, 0));
}

TEST_CASE("spy_mission_ai_kidnap_and_bribe_join_the_pinned_uniform_pick", "[ctest][spy-mission-ai]")
{
    spy::CityFacts city;
    city.kidnapReachable = true;
    CHECK(spy::EligibleMissions(city) == 0x11);
    city.kidnapReachable = false;
    city.bribeReachable  = true;
    CHECK(spy::EligibleMissions(city) == 0x41);
    city.kidnapReachable = true;
    city.currentItem     = 0;
    city.buildings       = 0x2;
    city.goldKnown       = true;
    city.population      = 3;
    city.reserve         = 100;
    city.statistic       = 1;
    CHECK(spy::EligibleMissions(city) == 0x7B);
    CHECK(spy::DescribeMissions(0x7B) == "0,1,3,4,5,6");

    // Six eligible: the hash range splits into six nearly equal intervals.
    const std::array<std::pair<uint32_t, int32_t>, 9> sixths{ { { 0x00000000, 0 },
                                                                { 0x2AAAAAAA, 0 },
                                                                { 0x2AAAAAAB, 1 },
                                                                { 0x55555555, 1 },
                                                                { 0x55555556, 3 },
                                                                { 0x80000000, 4 },
                                                                { 0xAAAAAAAA, 4 },
                                                                { 0xAAAAAAAB, 5 },
                                                                { 0xFFFFFFFF, 6 } } };
    for (const auto& [hash, mission] : sixths)
    {
        UNSCOPED_INFO("hash=" << std::hex << hash);
        CHECK(spy::ChooseMission(0x7B, hash) == mission);
    }
    CHECK(spy::ChooseMission(0x11, 0xB996BF0F) == 4);
    CHECK(spy::ChooseMission(0x41, 0xB996BF0F) == 6);
    CHECK(spy::ChooseMission(0x7B, 0xB996BF0F) == 5);

    std::map<int32_t, int32_t> counts;
    for (uint32_t turn = 1; turn <= 400; ++turn)
    {
        ++counts[spy::ChooseMission(0x7B, spy::ChoiceHash(turn, 1, 5, 7))];
    }
    CHECK((counts == std::map<int32_t, int32_t>{ { 0, 81 }, { 1, 61 }, { 3, 60 }, { 4, 68 }, { 5, 59 }, { 6, 71 } }));
}

namespace
{

// Every mission the hook picks for turns 1..12 (actor 1, city 5, slot 7).
std::set<int32_t> picksOverTurns(World& world)
{
    std::set<int32_t> picked;
    for (uint32_t turn = 1; turn <= 12; ++turn)
    {
        world.Turn(turn);
        Registers registers;
        registers.Run();
        picked.insert(missionOf(registers.selector));
    }
    return picked;
}

constexpr uint32_t kOwnerBank = kOwner;

} // namespace

TEST_CASE("spy_mission_ai_hook_offers_kidnap_only_for_a_reachable_great_person", "[ctest][spy-mission-ai]")
{
    World world;
    world.CityPosition();
    world.OpenKidnapGate();
    world.Rng();
    const std::set<int32_t> none{ 0 };
    const std::set<int32_t> kidnap{ 0, 4 };

    // No victim, then one accepted Great Person.
    CHECK(picksOverTurns(world) == none);
    world.Unit(kOwnerBank, 100, kOwner, 33, 0x4);
    {
        const auto before = world.Snapshot();
        CHECK(picksOverTurns(world) == kidnap);
        CHECK((world.Snapshot() == before));
        world.Turn(3);
        Registers registers;
        registers.Run();
        CHECK(registers.selector.s64 == spy::PackSelector(4, kCity));
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
    }
    // The first and last sampled slots count.
    world.Unit(kOwnerBank, 100, 0xFF, 0, 0);
    for (const uint32_t slot : { 0u, 255u })
    {
        world.Unit(kOwnerBank, slot, kOwner, 31, 0x4);
        CHECK(picksOverTurns(world) == kidnap);
        world.Unit(kOwnerBank, slot, 0xFF, 0, 0);
    }
    CHECK(picksOverTurns(world) == none);

    // Each loop test rejects the candidate.
    world.Unit(kOwnerBank, 100, kOwner, 33, 0x4);
    REQUIRE(picksOverTurns(world) == kidnap);
    const std::array<std::pair<const char*, std::array<uint32_t, 5>>, 8> rejected{ { { "General", { kOwner, 30, 0x4, kCityX, kCityY } },
                                                                                     { "type without a creator", { kOwner, 38, 0x4, kCityX, kCityY } },
                                                                                     { "x", { kOwner, 33, 0x4, kCityX + 1, kCityY } },
                                                                                     { "y", { kOwner, 33, 0x4, kCityX, kCityY - 1 } },
                                                                                     { "flag 4", { kOwner, 33, 0x0, kCityX, kCityY } },
                                                                                     { "dead", { kOwner, 33, 0x80000004, kCityX, kCityY } },
                                                                                     { "empty owner", { 0xFF, 33, 0x4, kCityX, kCityY } },
                                                                                     { "negative type", { kOwner, 0xFF, 0x4, kCityX, kCityY } } } };
    for (const auto& [name, record] : rejected)
    {
        UNSCOPED_INFO(name);
        world.Unit(kOwnerBank, 100, record[0], record[1], record[2], record[3], record[4]);
        CHECK(picksOverTurns(world) == none);
    }

    // Other banks are not sampled; slot 256 aliases the next bank's slot 0.
    world.Unit(kOwnerBank, 100, 0xFF, 0, 0);
    world.Unit(kOwnerBank + 1, 0, kOwner, 33, 0x4);
    world.Unit(kOwnerBank - 1, 100, kOwner, 33, 0x4);
    world.Unit(kOwnerBank, 256, kOwner, 33, 0x4);
    CHECK(picksOverTurns(world) == none);
    world.Unit(kOwnerBank, 255, kOwner, 33, 0x4);
    CHECK(picksOverTurns(world) == kidnap);
    world.Unit(kOwnerBank, 255, 0xFF, 0, 0);

    // The menu gate and the generator are both required.
    world.Unit(kOwnerBank, 100, kOwner, 33, 0x4);
    REQUIRE(picksOverTurns(world) == kidnap);
    world.Word(kClaims, 0);
    CHECK(picksOverTurns(world) == none);
    world.Half(kProtectGroups, 1);
    CHECK(picksOverTurns(world) == kidnap);
    world.Half(kProtectGroups, 0);
    world.Word(kClaims, 0xFFFFFFFF);
    world.Half(kCityRecord + 0x3C, 0);
    CHECK(picksOverTurns(world) == none);
    world.Half(kCityRecord + 0x3C, 1);
    REQUIRE(picksOverTurns(world) == kidnap);
    world.Rng(spy::kRngModulus + 1);
    CHECK(picksOverTurns(world) == none);
    world.Rng(spy::kRngModulus, spy::kRngNormalization + 1);
    CHECK(picksOverTurns(world) == none);
    world.Rng(0, 0);
    CHECK(picksOverTurns(world) == none);
    world.Rng();
    CHECK(picksOverTurns(world) == kidnap);
}

TEST_CASE("spy_mission_ai_hook_offers_bribe_only_for_a_reachable_general", "[ctest][spy-mission-ai]")
{
    World world;
    world.CityPosition();
    world.OpenBribeGate();
    world.Rng();
    const std::set<int32_t> none{ 0 };
    const std::set<int32_t> bribe{ 0, 6 };

    CHECK(picksOverTurns(world) == none);
    world.Unit(kOwnerBank, 40, kOwner, 30, 0);
    {
        const auto before = world.Snapshot();
        CHECK(picksOverTurns(world) == bribe);
        CHECK((world.Snapshot() == before));
        world.Turn(3);
        Registers registers;
        registers.Run();
        CHECK(registers.selector.s64 == spy::PackSelector(6, kCity));
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
    }

    const std::array<std::pair<const char*, std::array<uint32_t, 5>>, 7> rejected{ { { "Great Person", { kOwner, 31, 0, kCityX, kCityY } },
                                                                                     { "type", { kOwner, 29, 0, kCityX, kCityY } },
                                                                                     { "x", { kOwner, 30, 0, kCityX - 1, kCityY } },
                                                                                     { "y", { kOwner, 30, 0, kCityX, kCityY + 1 } },
                                                                                     { "dead", { kOwner, 30, 0x80000000, kCityX, kCityY } },
                                                                                     { "empty owner", { 0xFF, 30, 0, kCityX, kCityY } },
                                                                                     { "negative type", { kOwner, 0xE2, 0, kCityX, kCityY } } } };
    for (const auto& [name, record] : rejected)
    {
        UNSCOPED_INFO(name);
        world.Unit(kOwnerBank, 40, record[0], record[1], record[2], record[3], record[4]);
        CHECK(picksOverTurns(world) == none);
    }
    world.Unit(kOwnerBank, 40, 0xFF, 0, 0);
    world.Unit(kOwnerBank + 1, 0, kOwner, 30, 0);
    world.Unit(kOwnerBank - 1, 40, kOwner, 30, 0);
    CHECK(picksOverTurns(world) == none);
    for (const uint32_t slot : { 0u, 255u })
    {
        world.Unit(kOwnerBank, slot, kOwner, 30, 0x4);
        CHECK(picksOverTurns(world) == bribe);
        world.Unit(kOwnerBank, slot, 0xFF, 0, 0);
    }

    world.Unit(kOwnerBank, 40, kOwner, 30, 0);
    REQUIRE(picksOverTurns(world) == bribe);
    world.Half(kGeneralGate, 0);
    CHECK(picksOverTurns(world) == none);
    world.Half(kGeneralGate, 1);
    world.Half(kPresencePlane + 2 * (40 * kCityX + kCityY), 0);
    CHECK(picksOverTurns(world) == none);
    world.Half(kPresencePlane + 2 * (40 * kCityX + kCityY), 1);
    world.Rng(spy::kRngModulus - 1);
    CHECK(picksOverTurns(world) == none);
    world.Rng();
    CHECK(picksOverTurns(world) == bribe);
    // A city off the 40 x 40 plane has no presence word to test.
    world.CityPosition(40, kCityY);
    world.Unit(kOwnerBank, 40, kOwner, 30, 0, 40, kCityY);
    CHECK(picksOverTurns(world) == none);
}

TEST_CASE("spy_mission_ai_hook_picks_among_all_six_missions", "[ctest][spy-mission-ai]")
{
    World              world;
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%l %v");
    rex::AddSink(sink);

    struct RemoveSink
    {
        spdlog::sink_ptr sink;

        ~RemoveSink()
        {
            rex::RemoveSink(sink);
        }
    } removeSink{ sink };

    world.CityPosition();
    world.OpenKidnapGate();
    world.OpenBribeGate();
    world.Rng();
    world.Production(true);
    world.Buildings(0x2);
    world.StealableGold();
    world.Unit(kOwnerBank, 100, kOwner, 33, 0x4);
    world.Unit(kOwnerBank, 101, kOwner, 30, 0);

    const std::array<int32_t, 12> expected{ 1, 3, 5, 0, 1, 0, 6, 6, 5, 1, 1, 1 };
    for (uint32_t turn = 1; turn <= expected.size(); ++turn)
    {
        UNSCOPED_INFO("turn=" << turn);
        world.Turn(turn);
        const auto before = world.Snapshot();
        Registers  registers;
        registers.Run();
        CHECK(registers.selector.s64 == spy::PackSelector(expected[turn - 1], kCity));
        CHECK(registers.actor.u64 == kActorBits);
        CHECK(registers.unit.u64 == kUnitBits);
        CHECK((world.Snapshot() == before));
    }
    world.Turn(27);
    const auto before = world.Snapshot();
    Registers  registers;
    registers.Run();
    CHECK(registers.selector.s64 == spy::PackSelector(4, kCity));
    CHECK((world.Snapshot() == before));
    rex::FlushLogging();
    CHECK(output.str().find("info GameplayFix: spy mission actor=1 city=5 owner=2 unit=7 turn=27 eligible=0,1,3,4,5,6 chosen=4") != std::string::npos);
    CHECK(output.str().find("turn=7 eligible=0,1,3,4,5,6 chosen=6") != std::string::npos);
}
