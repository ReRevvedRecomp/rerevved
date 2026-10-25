#include "ai_economy_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

void ApplyAiResearchCostPercent(PPCRegister& technology, PPCRegister& player, PPCRegister& cost);
void ApplyAiUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& unitType, PPCRegister& scalar);
void ApplyAiNonUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& itemOffset, PPCRegister& cost);

namespace
{

namespace economy = rerevved::ai_economy;

struct World
{
    rex::Runtime runtime{ std::filesystem::path{} };

    World()
    {
        economy::ResetForLoad();
        economy::FinishLoad(false);
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const uint32_t page : { 0x82F70000u, 0x83120000u, 0x83140000u })
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        Word(0x82F700AC, 6);
        Word(0x82F700B0, 5);
        Word(0x8312E608, 1);
        Byte(0x8314F10B, 0);
        Byte(0x8314FE28 + 0xBC, 1);
        Byte(0x82F700D8 + 6 * 0x94 + 0x44, 2);
        economy::RestoreForGame({ 33, 50 });
    }

    ~World()
    {
        economy::ResetForLoad();
        economy::FinishLoad(false);
    }

    void Word(uint32_t address, uint32_t value)
    {
        rex::memory::store_and_swap<uint32_t>(runtime.memory()->TranslateVirtual(address), value);
    }

    void Byte(uint32_t address, uint8_t value)
    {
        *runtime.memory()->TranslateVirtual<uint8_t*>(address) = value;
    }
};

constexpr uint64_t kOriginal = 0xAABBCCDD00000065ull;

} // namespace

TEST_CASE("ai_economy_research_hook", "[ctest][ai-economy]")
{
    World       world;
    PPCRegister technology{}, player{}, cost{};
    technology.u64 = 0x1234567800000004ull;
    player.u64     = 0x8765432100000001ull;
    cost.u64       = kOriginal;
    ApplyAiResearchCostPercent(technology, player, cost);
    CHECK(cost.s64 == 33);
    CHECK(technology.u64 == 0x1234567800000004ull);
    CHECK(player.u64 == 0x8765432100000001ull);

    for (const int32_t id : { 0, 5, -1 })
    {
        player.s64 = id;
        cost.u64   = kOriginal;
        ApplyAiResearchCostPercent(technology, player, cost);
        CHECK(cost.u64 == kOriginal);
    }
    player.s64 = 1;
    for (const int32_t id : { -1, 0, 48 })
    {
        technology.s64 = id;
        cost.u64       = kOriginal;
        ApplyAiResearchCostPercent(technology, player, cost);
        CHECK(cost.u64 == kOriginal);
    }
    technology.s64 = 4;
    for (const uint32_t humans : { 0u, 2u })
    {
        world.Word(0x8312E608, humans);
        cost.u64 = kOriginal;
        ApplyAiResearchCostPercent(technology, player, cost);
        CHECK(cost.u64 == kOriginal);
    }
    world.Word(0x8312E608, 1);
    world.Word(0x82F700AC, 8);
    cost.u64 = kOriginal;
    ApplyAiResearchCostPercent(technology, player, cost);
    CHECK(cost.u64 == kOriginal);
    world.Word(0x82F700AC, 6);
    world.Word(0x82F700B0, 4);
    ApplyAiResearchCostPercent(technology, player, cost);
    CHECK(cost.u64 == kOriginal);
    world.Word(0x82F700B0, 5);
    cost.s64 = -1;
    ApplyAiResearchCostPercent(technology, player, cost);
    CHECK(cost.s64 == -1);
    economy::RestoreForGame({ 1, 50 });
    cost.s64 = 1;
    ApplyAiResearchCostPercent(technology, player, cost);
    CHECK(cost.s64 == 1);
}

TEST_CASE("ai_economy_production_hooks", "[ctest][ai-economy]")
{
    World       world;
    PPCRegister city{}, unit{}, scalar{};
    city.u64   = 0x12345678000000BCull;
    unit.u64   = 0x8765432100000006ull;
    scalar.s64 = 5;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.s64 == 2);
    CHECK(city.u64 == 0x12345678000000BCull);
    CHECK(unit.u64 == 0x8765432100000006ull);

    world.Byte(0x82F700D8 + 6 * 0x94 + 0x44, 1);
    economy::RestoreForGame({ 33, 1 });
    scalar.s64 = 3;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.s64 == 2);
    world.Byte(0x82F700D8 + 6 * 0x94 + 0x44, 0xFF);
    scalar.u64 = kOriginal;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.u64 == kOriginal);
    world.Byte(0x82F700D8 + 6 * 0x94 + 0x44, 2);
    scalar.u64 = 0x123456787FFFFFFFull;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.u64 == 0x123456787FFFFFFFull);
    unit.s64   = 29;
    scalar.u64 = kOriginal;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.u64 == kOriginal);
    unit.s64 = 6;
    city.s64 = 128 * 0xBC;
    ApplyAiUnitProductionCostPercent(city, unit, scalar);
    CHECK(scalar.u64 == kOriginal);

    city.u64 = 0x12345678000000BCull;
    PPCRegister item{}, cost{};
    economy::RestoreForGame({ 33, 50 });
    for (const int32_t offset : { 22, 100, 132 })
    {
        item.u64 = 0x8765432100000000ull | static_cast<uint32_t>(offset);
        cost.s64 = 3;
        ApplyAiNonUnitProductionCostPercent(city, item, cost);
        CHECK(cost.s64 == 1);
        CHECK(item.u64 == (0x8765432100000000ull | static_cast<uint32_t>(offset)));
        CHECK(city.u64 == 0x12345678000000BCull);
    }
    for (const int32_t offset : { -1, 23, 133, 199 })
    {
        item.s64 = offset;
        cost.u64 = kOriginal;
        ApplyAiNonUnitProductionCostPercent(city, item, cost);
        CHECK(cost.u64 == kOriginal);
    }
    item.s64 = 0;
    world.Byte(0x8314FE28 + 0xBC, 0);
    cost.u64 = kOriginal;
    ApplyAiNonUnitProductionCostPercent(city, item, cost);
    CHECK(cost.u64 == kOriginal);
}

TEST_CASE("ai_economy_expanded_layout_hooks", "[ctest][ai-economy]")
{
    World       world;
    PPCRegister technology{}, player{}, city{}, unit{}, item{}, value{};
    technology.s64   = 4;
    city.s64         = 0xBC;
    unit.s64         = 6;
    item.s64         = 0;
    const auto check = [&](int32_t owner, bool ai)
    {
        player.s64 = owner;
        world.Byte(0x8314FE28 + 0xBC, static_cast<uint8_t>(owner));
        value.s64 = 101;
        ApplyAiResearchCostPercent(technology, player, value);
        CHECK(value.s64 == (ai ? 33 : 101));
        value.s64 = 5;
        ApplyAiUnitProductionCostPercent(city, unit, value);
        CHECK(value.s64 == (ai ? 2 : 5));
        value.s64 = 101;
        ApplyAiNonUnitProductionCostPercent(city, item, value);
        CHECK(value.s64 == (ai ? 50 : 101));
    };
    for (const uint32_t count : { 7u, 8u })
    {
        world.Word(0x82F700AC, count);
        world.Word(0x82F700B0, count - 1);
        world.Word(0x8312E608, 1);
        check(5, true);
        check(6, count == 8);
        check(0, false);
        check(static_cast<int32_t>(count - 1), false);
        check(8, false);
        world.Word(0x8312E608, 0x21);
        check(5, false);
        world.Word(0x8312E608, 1u << (count - 1));
        check(5, false);
        world.Word(0x8312E608, 1);
        world.Byte(0x8314F10B, 1);
        check(5, false);
        world.Byte(0x8314F10B, 0);
    }
    world.Word(0x82F700AC, 9);
    world.Word(0x82F700B0, 8);
    check(5, false);
}

TEST_CASE("ai_economy_native_and_loading_hooks", "[ctest][ai-economy]")
{
    World       world;
    PPCRegister technology{}, player{}, city{}, unit{}, item{}, value{};
    technology.s64          = 4;
    player.s64              = 1;
    city.s64                = 0xBC;
    unit.s64                = 6;
    item.s64                = 0;
    const auto expectNative = [&]
    {
        value.u64 = kOriginal;
        ApplyAiResearchCostPercent(technology, player, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiUnitProductionCostPercent(city, unit, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiNonUnitProductionCostPercent(city, item, value);
        CHECK(value.u64 == kOriginal);
    };
    world.Byte(0x8314F10B, 1);
    expectNative();
    CHECK(economy::ForSave(economy::NativeCostsRequired()) == economy::Policy{});
    world.Byte(0x8314F10B, 0);
    for (const uint32_t mask : { 0x20u, 0x80000001u })
    {
        world.Word(0x8312E608, mask);
        expectNative();
    }
    world.Word(0x8312E608, 1);
    economy::BeginLoad();
    economy::ResetForLoad();
    economy::RestoreForGame({ 33, 50 });
    expectNative();
    economy::FinishLoad(true);
    expectNative();
    economy::RestoreForGame({ 100, 100 });
    expectNative();
}
