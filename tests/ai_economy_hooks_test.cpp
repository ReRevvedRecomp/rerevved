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
void ApplyAiGoldBonus(PPCRegister& stack, PPCRegister& owner, PPCRegister& value);
void ApplyAiFoodBonus(PPCRegister& stack, PPCRegister& value);
void ApplyAiCultureBonus(PPCRegister& stack, PPCRegister& owner, PPCRegister& value);

namespace
{

namespace economy = rerevved::ai_economy;

// A research bonus of 200 reduces cost 101 to 33. A production bonus of 100 halves it.
constexpr economy::Policy kPolicy{ 200, 100, 50, 100, 200 };
constexpr uint32_t        kStackPage         = 0x83160000;
constexpr uint32_t        kReadOnlyStackPage = 0x83170000;
constexpr uint32_t        kStack             = kStackPage + 0x100;
constexpr uint32_t        kReadOnlyStack     = kReadOnlyStackPage + 0x100;

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
        for (const uint32_t page : { 0x82F70000u, 0x83120000u, 0x83140000u, kStackPage, kReadOnlyStackPage })
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
        economy::RestoreForGame(kPolicy);
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

    uint32_t ReadWord(uint32_t address)
    {
        return rex::memory::load_and_swap<uint32_t>(runtime.memory()->TranslateVirtual(address));
    }

    void ProtectReadOnly(uint32_t page)
    {
        auto* heap = runtime.memory()->LookupHeap(page);
        REQUIRE((heap && heap->Protect(page, 0x1000, rex::memory::kMemoryProtectRead)));
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
    economy::RestoreForGame({ 200, 100 });
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
    economy::RestoreForGame({ 33, 200 });
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
    economy::RestoreForGame(kPolicy);
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
    PPCRegister technology{}, player{}, city{}, unit{}, item{}, value{}, stack{};
    technology.s64          = 4;
    player.s64              = 1;
    city.s64                = 0xBC;
    unit.s64                = 6;
    item.s64                = 0;
    stack.u64               = kStack;
    const auto expectNative = [&]
    {
        value.u64 = kOriginal;
        ApplyAiResearchCostPercent(technology, player, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiUnitProductionCostPercent(city, unit, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiNonUnitProductionCostPercent(city, item, value);
        CHECK(value.u64 == kOriginal);
        world.Word(kStack + 0x84, 1);
        for (const uint32_t saved : { 0x7Cu, 0x90u, 0x78u })
        {
            world.Word(kStack + saved, 0x65);
        }
        value.u64 = kOriginal;
        ApplyAiGoldBonus(stack, player, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiFoodBonus(stack, value);
        CHECK(value.u64 == kOriginal);
        ApplyAiCultureBonus(stack, player, value);
        CHECK(value.u64 == kOriginal);
        for (const uint32_t saved : { 0x7Cu, 0x90u, 0x78u })
        {
            CHECK(world.ReadWord(kStack + saved) == 0x65);
        }
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
    economy::RestoreForGame(kPolicy);
    expectNative();
    economy::FinishLoad(true);
    expectNative();
    economy::RestoreForGame({});
    expectNative();
}

namespace
{

enum class Yield
{
    Gold,
    Food,
    Culture,
};

struct YieldHook
{
    Yield    kind;
    uint32_t savedOffset;
    int32_t  bonus;
};

constexpr YieldHook kYieldHooks[] = {
    { Yield::Gold, 0x7C, kPolicy.goldBonusPercent },
    { Yield::Food, 0x90, kPolicy.foodBonusPercent },
    { Yield::Culture, 0x78, kPolicy.cultureBonusPercent },
};

constexpr uint32_t kSentinel = 0x5A5A5A5A;
constexpr uint64_t kHigh     = 0xAABBCCDD00000000ull;

// Put the owner where the yield hook reads it and seed saved words with
// the native value or a sentinel.
struct YieldRun
{
    PPCRegister stack{}, owner{}, value{};
    uint64_t    nativeValue = 0;

    YieldRun(World& world, const YieldHook& hook, int32_t ownerId, int32_t native, uint32_t stackBase = kStack)
    : nativeValue(kHigh | static_cast<uint32_t>(native))
    {
        stack.u64 = 0x1234567800000000ull | stackBase;
        owner.s64 = ownerId;
        value.u64 = nativeValue;
        // Food reads its owner from the word at r1+0x84, never from r4.
        world.Word(stackBase + 0x84, static_cast<uint32_t>(hook.kind == Yield::Food ? ownerId : 99));
        for (const uint32_t saved : { 0x78u, 0x7Cu, 0x90u })
        {
            world.Word(stackBase + saved, kSentinel);
        }
        world.Word(stackBase + hook.savedOffset, static_cast<uint32_t>(native));
        switch (hook.kind)
        {
            case Yield::Gold:
                ApplyAiGoldBonus(stack, owner, value);
                break;
            case Yield::Food:
                ApplyAiFoodBonus(stack, value);
                break;
            case Yield::Culture:
                ApplyAiCultureBonus(stack, owner, value);
                break;
        }
    }
};

} // namespace

TEST_CASE("ai_economy_yield_hooks_scale_register_and_saved_word", "[ctest][ai-economy]")
{
    World world;
    for (const auto& hook : kYieldHooks)
    {
        CAPTURE(static_cast<int>(hook.kind));
        const int32_t scaled = economy::ScaleYield(10, hook.bonus);
        REQUIRE(scaled > 10);
        YieldRun run(world, hook, 1, 10);
        CHECK(run.value.u64 == static_cast<uint32_t>(scaled));
        CHECK(world.ReadWord(kStack + hook.savedOffset) == static_cast<uint32_t>(scaled));
        CHECK(run.stack.u64 == (0x1234567800000000ull | kStack));
        for (const uint32_t other : { 0x78u, 0x7Cu, 0x90u })
        {
            if (other != hook.savedOffset)
            {
                CHECK(world.ReadWord(kStack + other) == kSentinel);
            }
        }

        // The scaled value is the cap, not a wrapped halfword.
        YieldRun capped(world, hook, 1, 30000);
        CHECK(capped.value.u64 == 32767);
        CHECK(world.ReadWord(kStack + hook.savedOffset) == 32767);
    }
}

TEST_CASE("ai_economy_yield_hooks_leave_native_values", "[ctest][ai-economy]")
{
    World world;
    for (const auto& hook : kYieldHooks)
    {
        CAPTURE(static_cast<int>(hook.kind));
        const auto expectNative = [&](int32_t owner, int32_t native)
        {
            YieldRun run(world, hook, owner, native);
            CHECK(run.value.u64 == run.nativeValue);
            CHECK(world.ReadWord(kStack + hook.savedOffset) == static_cast<uint32_t>(native));
        };
        expectNative(0, 10); // human
        expectNative(5, 10); // Barbarian
        expectNative(8, 10); // outside the player count
        expectNative(1, 0);
        expectNative(1, -7);
        expectNative(1, 32768);
        expectNative(1, std::numeric_limits<int32_t>::max());

        world.Byte(0x8314F10B, 1);
        expectNative(1, 10);
        world.Byte(0x8314F10B, 0);

        economy::RestoreForGame({});
        expectNative(1, 10);
        economy::RestoreForGame(kPolicy);

        // The same inputs scale again once the policy is back.
        YieldRun scaled(world, hook, 1, 10);
        CHECK(scaled.value.u64 != scaled.nativeValue);
    }
}

TEST_CASE("ai_economy_food_hook_owner_comes_from_stack", "[ctest][ai-economy]")
{
    World       world;
    PPCRegister stack{}, value{};
    stack.u64 = kStack;

    // The word at r1+0x84 is the owner. Other words do not matter.
    for (const uint32_t saved : { 0x78u, 0x7Cu, 0x90u })
    {
        world.Word(kStack + saved, 10);
    }
    value.s64 = 10;
    world.Word(kStack + 0x84, 0);
    ApplyAiFoodBonus(stack, value);
    CHECK(value.s64 == 10);
    CHECK(world.ReadWord(kStack + 0x90) == 10);
    world.Word(kStack + 0x84, 1);
    ApplyAiFoodBonus(stack, value);
    CHECK(value.s64 == 20);
    CHECK(world.ReadWord(kStack + 0x90) == 20);

    // A negative owner word is not an ordinary AI owner.
    value.s64 = 10;
    world.Word(kStack + 0x84, 0xFFFFFFFFu);
    ApplyAiFoodBonus(stack, value);
    CHECK(value.s64 == 10);
}

TEST_CASE("ai_economy_yield_hooks_require_writable_saved_word", "[ctest][ai-economy]")
{
    World world;
    // Fill the page while it is writable, then make it read only.
    for (const uint32_t saved : { 0x78u, 0x7Cu, 0x90u })
    {
        world.Word(kReadOnlyStack + saved, 10);
    }
    world.Word(kReadOnlyStack + 0x84, 1);
    world.ProtectReadOnly(kReadOnlyStackPage);

    for (const auto& hook : kYieldHooks)
    {
        CAPTURE(static_cast<int>(hook.kind));
        for (const uint32_t stackBase : { kReadOnlyStack, 0x10000u })
        {
            PPCRegister stack{}, owner{}, value{};
            stack.u64 = stackBase;
            owner.s64 = 1;
            value.u64 = kHigh | 10;
            switch (hook.kind)
            {
                case Yield::Gold:
                    ApplyAiGoldBonus(stack, owner, value);
                    break;
                case Yield::Food:
                    ApplyAiFoodBonus(stack, value);
                    break;
                case Yield::Culture:
                    ApplyAiCultureBonus(stack, owner, value);
                    break;
            }
            CHECK(value.u64 == (kHigh | 10));
        }
        CHECK(world.ReadWord(kReadOnlyStack + hook.savedOffset) == 10);
    }
}
