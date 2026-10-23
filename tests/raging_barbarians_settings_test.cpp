#include "cvar_scope.hpp"
#include "raging_barbarians_hooks.h"
#include "raging_barbarians_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, raging_barbarians);

namespace
{

void expectEffective(bool enabled)
{
    for (const int32_t native : { -32768, -1, 0, 1, 2, 3, 32767 })
    {
        const int32_t expected   = enabled ? 2 : native;
        const auto    evaluation = rerevved::raging_barbarians::Evaluate(native);
        UNSCOPED_INFO("frozen selection controls the consumer input");
        REQUIRE((evaluation.nativeValue == native && evaluation.finalValue == expected && evaluation.enabled == enabled));

        PPCRegister fecundity{};
        PPCRegister base{};
        PPCRegister offset{};
        fecundity.s64 = native;
        base.u64      = 0x123456788314FE28ull;
        offset.u64    = 0x87654321000000BCull;
        ApplyRagingBarbariansFecundity(fecundity, base, offset);
        UNSCOPED_INFO("off retains native; on selects signed case 2");
        REQUIRE(fecundity.s64 == expected);
        UNSCOPED_INFO("observer inputs preserved");
        REQUIRE((base.u64 == 0x123456788314FE28ull && offset.u64 == 0x87654321000000BCull));

        PPCRegister aggression{};
        PPCRegister cityOrdinal{};
        // The native lhz supplies unsigned bits to both signed extsh consumers.
        aggression.u64          = static_cast<uint16_t>(native);
        cityOrdinal.u64         = 0x123456780000007Full;
        const uint64_t original = aggression.u64;
        ApplyRagingBarbariansAggression(aggression, cityOrdinal);
        UNSCOPED_INFO("aggression off preserves every bit; on selects level 2");
        REQUIRE(aggression.u64 == (enabled ? 2 : original));
        UNSCOPED_INFO("both signed aggression consumers receive the effective level");
        REQUIRE(aggression.s16 == expected);
        UNSCOPED_INFO("aggression preserves its city ordinal");
        REQUIRE(cityOrdinal.u64 == 0x123456780000007Full);
        UNSCOPED_INFO("aggression leaves the fecundity input unchanged");
        REQUIRE(fecundity.s64 == expected);
    }
}

void testDepartureThreshold()
{
    // Replay 0x82CB3F44..0x82CB3F6C and the signed comparison at 0x82CC1308.
    for (const bool enabled : { false, true })
    {
        REXCVAR_SET(raging_barbarians, enabled);
        FreezeRagingBarbariansForGame();
        for (const int32_t nativeFecundity : { 0, 1, 2, 3 })
        {
            for (const int32_t nativeAggression : { 0, 1, 2 })
            {
                PPCRegister aggression{};
                PPCRegister cityOrdinal{};
                PPCRegister fecundity{};
                PPCRegister base{};
                PPCRegister offset{};
                aggression.u64 = nativeAggression;
                fecundity.s64  = nativeFecundity;
                ApplyRagingBarbariansAggression(aggression, cityOrdinal);
                ApplyRagingBarbariansFecundity(fecundity, base, offset);
                UNSCOPED_INFO("aggression is independent of native fecundity");
                REQUIRE(aggression.s16 == (enabled ? 2 : nativeAggression));
                UNSCOPED_INFO("fecundity is independent of native aggression");
                REQUIRE(fecundity.s32 == (enabled ? 2 : nativeFecundity));
                const int32_t threshold = aggression.s16 == 0 ? 99 : 3 - aggression.s16;
                UNSCOPED_INFO("native zero/99 and 3-a branches consume the aggression override");
                REQUIRE(threshold == (enabled ? 1 : nativeAggression == 0 ? 99
                                                                          : 3 - nativeAggression));
                if (nativeAggression != 1)
                {
                    continue;
                }
                for (const int32_t count : { 1, 2, 3 })
                {
                    PPCRegister defenders{};
                    PPCRegister result{};
                    PPCRegister origin{};
                    defenders.s64 = count;
                    result.s64    = threshold;
                    origin.u64    = 0x123456780000003Full;
                    ObserveRagingBarbariansDeparture(defenders, result, base, offset, origin);
                    UNSCOPED_INFO("departure observer preserves all guest inputs");
                    REQUIRE((defenders.s64 == count && result.s64 == threshold && base.u64 == 0 && offset.u64 == 0 &&
                             origin.u64 == 0x123456780000003Full));
                    const bool held = defenders.s32 <= result.s32;
                    UNSCOPED_INFO("level 2 uses R=1; native level 1 uses R=2");
                    REQUIRE(held == (enabled ? count <= 1 : count <= 2));
                }
            }
        }
    }
    REXCVAR_SET(raging_barbarians, false);
    FreezeRagingBarbariansForGame();
}

void testFrozenSelection()
{
    UNSCOPED_INFO("title config defaults off");
    REQUIRE(!REXCVAR_GET(raging_barbarians));
    expectEffective(false);
    REXCVAR_SET(raging_barbarians, true);
    expectEffective(false);

    // The new-game ingress snapshots the pending checkbox.
    FreezeRagingBarbariansForGame();
    expectEffective(true);
    REXCVAR_SET(raging_barbarians, false);
    expectEffective(true);

    // Save-load ingress uses the same snapshot, without a save sidecar.
    FreezeRagingBarbariansForGame();
    expectEffective(false);
    REXCVAR_SET(raging_barbarians, true);
    expectEffective(false);
    FreezeRagingBarbariansForGame();
    expectEffective(true);

    // A later new game can also turn the frozen selection off.
    REXCVAR_SET(raging_barbarians, false);
    expectEffective(true);
    FreezeRagingBarbariansForGame();
    expectEffective(false);
}

void testStandardRules()
{
    REXCVAR_SET(raging_barbarians, true);
    rerevved::raging_barbarians::FreezeForGame(true);
    expectEffective(false);
    REXCVAR_SET(raging_barbarians, false);
    expectEffective(false);
    REXCVAR_SET(raging_barbarians, true);
    rerevved::raging_barbarians::FreezeForGame(false);
    expectEffective(true);
}

void testCreationResults()
{
    using rerevved::raging_barbarians::IsCreationResultValid;
    UNSCOPED_INFO("native allocation failure is not a unit");
    REQUIRE(!IsCreationResultValid(5, 3, -1));
    UNSCOPED_INFO("slot zero is valid");
    REQUIRE(IsCreationResultValid(5, 3, 0));
    UNSCOPED_INFO("last native slot is valid");
    REQUIRE(IsCreationResultValid(5, 5, 255));
    UNSCOPED_INFO("invalid slot cannot index guest memory");
    REQUIRE(!IsCreationResultValid(5, 3, 256));
    UNSCOPED_INFO("disabled barbarian ordinal is invalid");
    REQUIRE(!IsCreationResultValid(-1, 3, 0));
    UNSCOPED_INFO("invalid owner cannot index guest memory");
    REQUIRE(!IsCreationResultValid(6, 3, 0));
    UNSCOPED_INFO("other production types are outside diagnostic");
    REQUIRE(!IsCreationResultValid(5, 7, 0));
}

} // namespace

TEST_CASE("raging_barbarians_settings", "[ctest][raging-barbarians-settings][local-fast]")
{
    REREVVED_RESTORE_CVAR(raging_barbarians);
    testFrozenSelection();
    testDepartureThreshold();
    testCreationResults();
    testStandardRules();
}
