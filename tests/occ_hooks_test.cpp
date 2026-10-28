#include "callback_checks.hpp"
#include "cvar_scope.hpp"
#include "occ_hooks.h"
#include "occ_settings.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, one_city_challenge);
REXCVAR_DECLARE(bool, occ_trace);

namespace
{

namespace occ = rerevved::occ;

constexpr uint32_t kCities    = 0x8314FE28;
constexpr uint32_t kStride    = 0xBC;
constexpr uint32_t kStack     = 0x40008000;
constexpr uint32_t kMarker    = 0x40000020;
constexpr uint32_t kHumanMask = 0x8312E608;

struct World;
thread_local World* active = nullptr;
void                GrowthHelper(PPCContext& context, uint8_t* base);

struct World
{
    rex::Runtime                               runtime{ std::filesystem::path{} };
    const occ::Policy                          savedPolicy{ *occ::ForGame() };
    rex::runtime::ThreadState*                 previousThread = rex::runtime::ThreadState::Get();
    std::unique_ptr<rex::runtime::ThreadState> thread;
    rerevved::tests::CallbackChecks            checks;
    std::array<uint32_t, occ::kCitySlots>      refreshes{};

    World()
    {
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const uint32_t page : { 0x40000000u, 0x83120000u, 0x83140000u, 0x83150000u })
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        auto* dispatcher = runtime.function_dispatcher();
        REQUIRE(dispatcher->InitializeFunctionTable(0x82D00000, 0x10000, 0x82D00000, 0x10000, true));
        REQUIRE(dispatcher->SetFunction(0x82D00C78, GrowthHelper));
        std::memset(Base() + kCities, 0xFF, occ::kCitySlots * kStride);
        std::memset(Base() + kStack, 0xA5, 400);
        Word(kMarker, 1);
        Word(kHumanMask, 1);
        City(0, 1, 1);
        City(1, 1, 30);
        City(2, 1, 31);
        City(3, 0, 9);
        City(4, 2, 9);
        City(5, 1, 0);
        City(127, 1, 12);
        thread          = std::make_unique<rex::runtime::ThreadState>(1, kStack, 0, runtime.memory());
        auto& context   = *thread->context();
        context.r3.u64  = 0x123456789ABCDEF0ull;
        context.r14.u64 = kMarker;
        context.r15.u64 = 1;
        context.r22.u64 = 1;
        context.lr      = 0x82CF5760;
        active          = this;
        rex::runtime::ThreadState::Bind(thread.get());
    }

    ~World()
    {
        rex::runtime::ThreadState::Bind(previousThread);
        active = nullptr;
        occ::RestoreForGame(savedPolicy);
    }

    uint8_t* Base()
    {
        return runtime.memory()->virtual_membase();
    }

    void Word(uint32_t address, uint32_t value)
    {
        rex::memory::store_and_swap<uint32_t>(Base() + address, value);
    }

    uint32_t Word(uint32_t address)
    {
        return rex::memory::load_and_swap<uint32_t>(Base() + address);
    }

    void City(uint32_t index, uint8_t owner, uint8_t population)
    {
        Base()[kCities + index * kStride]     = owner;
        Base()[kCities + index * kStride + 3] = population;
    }

    bool Apply()
    {
        auto& context = *thread->context();
        return ApplyOccGoldReward(context.r22, context.r15);
    }
};

void GrowthHelper(PPCContext& context, uint8_t* base)
{
    auto&      world = *active;
    const auto city  = context.r3.u32;
    world.checks.Require(city < occ::kCitySlots, "growth helper receives a valid city ordinal");
    world.checks.Require(base[kCities + city * kStride] == 1, "growth helper receives only the rewarded AI's cities");
    world.checks.Require(++world.refreshes[city] == 1, "growth helper runs once per eligible city");
    context.r3.u64  = 0;
    context.r14.u64 = 0;
    context.r15.u64 = 0;
    context.r22.u64 = 0;
}

} // namespace

TEST_CASE("occ_applying_ai_reward_hook", "[ctest][occ-settings]")
{
    REREVVED_RESTORE_CVAR(one_city_challenge);
    World world;
    REXCVAR_SET(one_city_challenge, true);
    FreezeOccForGame();
    auto&                                          context = *world.thread->context();
    std::array<uint8_t, occ::kCitySlots * kStride> expected{};
    std::memcpy(expected.data(), world.Base() + kCities, expected.size());
    std::array<uint8_t, 400> frame{};
    std::memcpy(frame.data(), world.Base() + kStack, frame.size());

    context.r15.u64 = 0;
    UNSCOPED_INFO("AI preview skips the native Settler branch without changing cities");
    REQUIRE(world.Apply());
    CHECK(std::memcmp(expected.data(), world.Base() + kCities, expected.size()) == 0);
    CHECK((world.refreshes == std::array<uint32_t, occ::kCitySlots>{}));

    context.r15.u64 = 1;
    UNSCOPED_INFO("the applying AI hook selects the continuation after the native Settler grant");
    REQUIRE(world.Apply());
    world.checks.Verify();
    expected[3]                 = 2;
    expected[kStride + 3]       = 31;
    expected[127 * kStride + 3] = 13;
    CHECK(std::memcmp(expected.data(), world.Base() + kCities, expected.size()) == 0);
    std::array<uint32_t, occ::kCitySlots> expectedRefreshes{};
    expectedRefreshes[0]   = 1;
    expectedRefreshes[1]   = 1;
    expectedRefreshes[127] = 1;
    CHECK(world.refreshes == expectedRefreshes);
    CHECK(world.Word(kHumanMask) == 1);
    CHECK(world.Word(kMarker) == 1);
    CHECK(std::memcmp(frame.data(), world.Base() + kStack, frame.size()) == 0);
    CHECK(context.r1.u64 == kStack);
    CHECK(context.r3.u64 == 0x123456789ABCDEF0ull);
    CHECK(context.r14.u64 == kMarker);
    CHECK(context.r15.u64 == 1);
    CHECK(context.r22.u64 == 1);
    CHECK(context.lr == 0x82CF5760);

    occ::RestoreForGame({ false, occ::kMinimumPopulation });
    UNSCOPED_INFO("a restored disabled policy retains the native Settler branch despite a checked setting");
    CHECK(REXCVAR_GET(one_city_challenge));
    CHECK_FALSE(world.Apply());
    CHECK(std::memcmp(expected.data(), world.Base() + kCities, expected.size()) == 0);
    CHECK(world.refreshes == expectedRefreshes);

    FreezeOccForGame();
    UNSCOPED_INFO("the next ordinary new-game freeze uses the checked setting again");
    CHECK(occ::ForGame()->enabled);
}

TEST_CASE("occ_village_settlers_hook", "[ctest][occ-settings]")
{
    REREVVED_RESTORE_CVAR(one_city_challenge);
    World world;
    auto& context   = *world.thread->context();
    context.r27.u64 = 0x1234567800000003ull;

    occ::RestoreForGame({ false, occ::kMinimumPopulation });
    UNSCOPED_INFO("disabled OCC keeps the native village Settlers branch");
    CHECK_FALSE(GuardOccVillageSettlers(context.r27));
    CHECK(context.r27.u64 == 0x1234567800000003ull);

    occ::RestoreForGame({ true, occ::kMinimumPopulation });
    UNSCOPED_INFO("enabled OCC selects the native Gold rejection path without changing the player");
    CHECK(GuardOccVillageSettlers(context.r27));
    CHECK(context.r27.u64 == 0x1234567800000003ull);
}

TEST_CASE("occ_ai_expansion_hook", "[ctest][occ-settings]")
{
    REREVVED_RESTORE_CVAR(one_city_challenge);
    REREVVED_RESTORE_CVAR(occ_trace);
    World world;
    auto& context = *world.thread->context();
    world.Word(kStack + 0x1C4, 3);
    context.r1.u64             = kStack;
    context.r24.u64            = 0x1234567800000011ull;
    context.r29.u64            = 0x8765432100000022ull;
    const auto expectUnchanged = [&]
    {
        CHECK(context.r1.u64 == kStack);
        CHECK(context.r24.u64 == 0x1234567800000011ull);
        CHECK(context.r29.u64 == 0x8765432100000022ull);
        CHECK(world.Word(kStack + 0x1C4) == 3);
    };

    occ::RestoreForGame({ false, occ::kMinimumPopulation });
    UNSCOPED_INFO("disabled OCC keeps the native AI expansion arm");
    CHECK_FALSE(GuardOccAiExpansion(context.r1, context.r24, context.r29));
    expectUnchanged();

    occ::RestoreForGame({ true, occ::kMinimumPopulation });
    UNSCOPED_INFO("enabled OCC skips the AI expansion arm without changing registers or memory");
    CHECK(GuardOccAiExpansion(context.r1, context.r24, context.r29));
    expectUnchanged();

    REXCVAR_SET(occ_trace, true);
    UNSCOPED_INFO("tracing the refusal does not change the result");
    CHECK(GuardOccAiExpansion(context.r1, context.r24, context.r29));
    expectUnchanged();

    context.r1.u64 = 0;
    UNSCOPED_INFO("an unreadable frame still refuses the expansion");
    CHECK(GuardOccAiExpansion(context.r1, context.r24, context.r29));
    CHECK(context.r1.u64 == 0);
    CHECK(context.r24.u64 == 0x1234567800000011ull);
    CHECK(context.r29.u64 == 0x8765432100000022ull);
}
