#include "ai_civilization_setup.h"

#include "ai_civilizations.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <stdexcept>

#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace
{

namespace ai = rerevved::ai_civilizations;

constexpr uint32_t kSpecialGame = 0x8314F180;

struct World
{
    rex::Runtime runtime{ std::filesystem::path{} };

    World()
    {
        ai::ResetOnLoad();
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const uint32_t page : { 0x82F70000u, 0x83120000u, 0x83140000u })
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        Layout(6, 5);
        Word(ai::kHumanMaskGlobal, 1);
        Word(kSpecialGame, 0);
        Word(0x8314F1CC, 0);
        Word(0x8314F1D0, 0);
        Base()[ai::kNetworkGameByte] = 0;
    }

    ~World()
    {
        ai::ResetOnLoad();
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

    void Layout(uint32_t count, uint32_t barbarian)
    {
        Word(ai::kPlayerCountGlobal, count);
        Word(ai::kBarbarianPlayerGlobal, barbarian);
    }

    void CheckLayout(uint32_t count, uint32_t barbarian)
    {
        CHECK(Word(ai::kPlayerCountGlobal) == count);
        CHECK(Word(ai::kBarbarianPlayerGlobal) == barbarian);
    }
};

void checkInactiveHooks()
{
    PPCRegister count{}, barbarian{};
    count.u64     = 0xAABBCCDD00000006ull;
    barbarian.u64 = 0x1122334400000005ull;
    ConfigureExpandedAiSetup();
    PreserveExpandedAiSetup(count, barbarian);
    CHECK(count.u64 == 0xAABBCCDD00000006ull);
    CHECK(barbarian.u64 == 0x1122334400000005ull);
    CHECK(ai::ExpandedAiLimit() == 0);
}

} // namespace

TEST_CASE("ai_civilization_setup_inactive", "[ctest][ai-civilizations]")
{
    World world;
    checkInactiveHooks();
    ai::CommitNewGame();
    for (const int32_t requested : { 0, 4, 7 })
    {
        ai::SetupScope scope(requested, false, world.Base());
        checkInactiveHooks();
        world.CheckLayout(6, 5);
    }
    checkInactiveHooks();
}

TEST_CASE("ai_civilization_setup_rejections", "[ctest][ai-civilizations]")
{
    World    world;
    bool     standardRules = false;
    int32_t  mode          = 0;
    uint8_t* base          = world.Base();
    uint32_t count         = 6;
    uint32_t barbarian     = 5;
    SECTION("Standard Game of the Week")
    {
        standardRules = true;
    }
    SECTION("network game")
    {
        world.Base()[ai::kNetworkGameByte] = 1;
    }
    SECTION("custom Game of the Week")
    {
        mode = 2;
    }
    SECTION("special native game")
    {
        world.Word(kSpecialGame, 1);
    }
    SECTION("first native selection flag")
    {
        world.Word(0x8314F1CC, 1u << 16);
    }
    SECTION("second native selection flag")
    {
        world.Word(0x8314F1D0, 1u << 16);
    }
    SECTION("missing human")
    {
        world.Word(ai::kHumanMaskGlobal, 0);
    }
    SECTION("multiple humans")
    {
        world.Word(ai::kHumanMaskGlobal, 3);
    }
    SECTION("human after an AI slot")
    {
        world.Word(ai::kHumanMaskGlobal, 0x10);
    }
    SECTION("Barbarian human bit")
    {
        world.Word(ai::kHumanMaskGlobal, 0x21);
    }
    SECTION("unsupported native layout")
    {
        count     = 2;
        barbarian = UINT32_MAX;
        world.Layout(count, barbarian);
    }
    SECTION("absent guest base")
    {
        base = nullptr;
    }
    ai::SetupScope scope(6, standardRules, base, mode);
    checkInactiveHooks();
    ai::CommitNewGame();
    world.CheckLayout(count, barbarian);
}

TEST_CASE("ai_civilization_setup_choices_retry_and_commit", "[ctest][ai-civilizations]")
{
    World world;
    for (const int32_t requested : { 5, 6 })
    {
        world.Word(ai::kHumanMaskGlobal, 0);
        // Adjacent halfwords do not belong to the native selection flags.
        world.Word(0x8314F1CC, 1);
        world.Word(0x8314F1D0, 1);
        {
            ai::SetupScope scope(requested, false, world.Base());
            world.CheckLayout(6, 5);
            CHECK(ai::ExpandedAiLimit() == 0);
            // The hook uses the mask rebuilt by native setup, not the mask at scope entry.
            world.Word(ai::kHumanMaskGlobal, 1);
            ConfigureExpandedAiSetup();
            world.CheckLayout(requested + 2, requested + 1);
            CHECK(ai::ExpandedAiLimit() == requested);
            ConfigureExpandedAiSetup();
            world.CheckLayout(requested + 2, requested + 1);
            PPCRegister count{}, barbarian{};
            count.u64     = 0xAABBCCDD00000006ull;
            barbarian.u64 = 0x1122334400000005ull;
            PreserveExpandedAiSetup(count, barbarian);
            CHECK(count.u64 == static_cast<uint32_t>(requested + 2));
            CHECK(barbarian.u64 == static_cast<uint32_t>(requested + 1));
            ai::CommitNewGame();
            ai::NoteNewGameRecount();
            ai::CommitNewGame();
            ai::CommitNewGame();
        }
        world.CheckLayout(requested + 2, requested + 1);
        checkInactiveHooks();
    }
}

TEST_CASE("ai_civilization_setup_failed_scope_rolls_back", "[ctest][ai-civilizations]")
{
    World world;
    SECTION("native setup returned before recount")
    {
        ai::SetupScope scope(6, false, world.Base());
        ai::CommitNewGame();
        ConfigureExpandedAiSetup();
        world.CheckLayout(8, 7);
    }
    SECTION("native setup threw")
    {
        try
        {
            ai::SetupScope scope(6, false, world.Base());
            ConfigureExpandedAiSetup();
            ai::NoteNewGameRecount();
            throw std::runtime_error("native setup interrupted");
        }
        catch (const std::runtime_error&)
        {
        }
    }
    SECTION("recount saw an inconsistent layout")
    {
        ai::SetupScope scope(6, false, world.Base());
        ConfigureExpandedAiSetup();
        world.Layout(7, 6);
        ai::NoteNewGameRecount();
        ai::CommitNewGame();
    }
    SECTION("native init returned without the setup recount")
    {
        ai::SetupScope scope(6, false, world.Base());
        ConfigureExpandedAiSetup();
        ai::CommitNewGame();
    }
    world.CheckLayout(6, 5);
    checkInactiveHooks();
}

TEST_CASE("ai_civilization_setup_normalizes_only_old_expansion", "[ctest][ai-civilizations]")
{
    World world;
    for (const int32_t next : { 4, 5, 6 })
    {
        for (const uint32_t previousCount : { 7u, 8u })
        {
            world.Layout(previousCount, previousCount - 1);
            world.Base()[ai::kNetworkGameByte] = 1;
            ai::SetupScope scope(next, false, world.Base());
            world.CheckLayout(6, 5);
            checkInactiveHooks();
        }
    }
    world.Layout(9, 8);
    {
        ai::SetupScope scope(4, false, world.Base());
        world.CheckLayout(9, 8);
    }
    world.CheckLayout(9, 8);
}

TEST_CASE("ai_civilization_setup_load_reset_keeps_native_layout", "[ctest][ai-civilizations]")
{
    World world;
    {
        ai::SetupScope scope(6, false, world.Base());
        ConfigureExpandedAiSetup();
        world.CheckLayout(8, 7);
        ai::ResetOnLoad();
        checkInactiveHooks();
        ai::CommitNewGame();
        world.CheckLayout(8, 7);
    }
    world.CheckLayout(8, 7);
    ai::ResetOnLoad();
    checkInactiveHooks();
    world.CheckLayout(8, 7);
}
