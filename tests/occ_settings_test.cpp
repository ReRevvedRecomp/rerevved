#include "cvar_scope.hpp"
#include "nation_select_text_registry.h"
#include "occ_hooks.h"
#include "occ_settings.h"
#include "registry_scope.hpp"
#include "unique_era_abilities_registry.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, one_city_challenge);

void ApplyUniqueEraAbilityCell(PPCRegister& cellOffset, PPCRegister& unlockEra, PPCRegister& nativeAbility);
void ApplyBarbarianVillageCityReplacement(PPCRegister& civilization);

namespace
{

namespace occ = rerevved::occ;

class Cities final : public occ::RewardCities
{
public:
    std::array<occ::City, occ::kCitySlots> records{};
    std::vector<int32_t>                   refreshed;
    std::vector<int32_t>                   observed;
    int32_t                                reads = 0;

    Cities()
    {
        records.fill({ -1, 0 });
    }

    occ::City Read(int32_t index) override
    {
        ++reads;
        return records.at(index);
    }

    void WritePopulation(int32_t index, int32_t population) override
    {
        UNSCOPED_INFO("reward increments once before the helper");
        REQUIRE(records.at(index).population + 1 == population);
        records.at(index).population = population;
    }

    void Refresh(int32_t index) override
    {
        refreshed.push_back(index);
    }

    void Observe(int32_t index, occ::City before, occ::City after) override
    {
        UNSCOPED_INFO("growth retains city ownership");
        REQUIRE(before.owner == after.owner);
        UNSCOPED_INFO("growth respects the cap");
        REQUIRE(after.population == before.population + (before.population < 31 ? 1 : 0));
        observed.push_back(index);
    }
};

// The frozen game policy is process-wide; put it back when the case ends.
class PolicyRestore
{
public:
    PolicyRestore()
    : saved_(*occ::ForGame())
    {
    }

    ~PolicyRestore()
    {
        occ::RestoreForGame(saved_);
    }

    PolicyRestore(const PolicyRestore&)            = delete;
    PolicyRestore& operator=(const PolicyRestore&) = delete;

private:
    occ::Policy saved_;
};

void testFrozenPolicy()
{
    UNSCOPED_INFO("checkbox defaults off");
    REQUIRE(!REXCVAR_GET(one_city_challenge));
    const auto initial = occ::ForGame();
    UNSCOPED_INFO("initial immutable policy defaults off with R=33");
    REQUIRE((!initial->enabled && initial->minimumPopulation == 33));
    REXCVAR_SET(one_city_challenge, true);
    UNSCOPED_INFO("pending checkbox does not replace game policy");
    REQUIRE(occ::ForGame() == initial);
    FreezeOccForGame();
    const auto started = occ::ForGame();
    UNSCOPED_INFO("new-game freeze publishes enabled/R together");
    REQUIRE((started != initial && started->enabled && started->minimumPopulation == 33));
    REXCVAR_SET(one_city_challenge, false);
    UNSCOPED_INFO("mid-game changes leave the same immutable record");
    REQUIRE((occ::ForGame() == started && started->enabled));
    UNSCOPED_INFO("both comparisons use the frozen pair after a pending change");
    REQUIRE((occ::CandidateThreshold(*occ::ForGame(), 0, 2) == 33 && occ::CompletionThreshold(*occ::ForGame(), 0, 1) == 32));
    FreezeOccForGame();
    const auto loaded = occ::ForGame();
    UNSCOPED_INFO("load freezes the pending off choice");
    REQUIRE((loaded != started && !loaded->enabled && loaded->minimumPopulation == 33));
    UNSCOPED_INFO("previous policy records stay immutable");
    REQUIRE((started->enabled && !initial->enabled));
    REXCVAR_SET(one_city_challenge, true);
    UNSCOPED_INFO("pending on choice waits for ingress");
    REQUIRE(occ::ForGame() == loaded);
    FreezeOccForGame();
    UNSCOPED_INFO("next game can turn OCC on again");
    REQUIRE(occ::ForGame()->enabled);
}

void testMongolHorsebackMatrix()
{
    constexpr EraAbilityId native    = ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES;
    constexpr EraAbilityId horseback = ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    for (bool enabled : { false, true })
    {
        for (bool modEnabled : { false, true })
        {
            rerevved::unique_era_abilities::ResetForTests();
            rerevved::nation_select_text::ResetForTests();
            if (modEnabled)
            {
                EraAbilityReplacement rule{};
                rule.structSize = sizeof(rule);
                std::strcpy(rule.providerId, "aeshur.mongol-horseback");
                std::strcpy(rule.ruleId, "mongol-ancient-horseback-riding");
                rule.civilization       = CIVILIZATION_MONGOLIAN;
                rule.unlockEra          = UNLOCK_ERA_ANCIENT;
                rule.replacementAbility = horseback;
                UNSCOPED_INFO("mod ability publishes");
                REQUIRE(RegisterEraAbilityReplacement(&rule) == ERA_ABILITIES_OK);

                NationSelectTextRule text{};
                text.structSize = sizeof(text);
                std::strcpy(text.providerId, rule.providerId);
                std::strcpy(text.ruleId, "mongol-ancient-horseback-riding-text");
                text.surface      = NATION_SELECT_TEXT_SURFACE_ERA_ABILITY;
                text.civilization = CIVILIZATION_MONGOLIAN;
                text.unlockEra    = UNLOCK_ERA_ANCIENT;
                text.ability      = horseback;
                text.baseUnitType = NATION_SELECT_TEXT_SELECTOR_UNUSED;
                text.identity     = NATION_SELECT_TEXT_SELECTOR_UNUSED;
                text.displayForm  = NATION_SELECT_TEXT_SELECTOR_UNUSED;
                std::strcpy(text.text, "Start with Horseback Riding");
                UNSCOPED_INFO("mod text publishes");
                REQUIRE(RegisterNationSelectTextRule(&text) == NATION_SELECT_TEXT_OK);
            }

            REXCVAR_SET(one_city_challenge, !enabled);
            FreezeOccForGame();
            const auto previousGame = occ::ForGame();
            REXCVAR_SET(one_city_challenge, enabled);
            NationSelectTextEvaluation presentation{};
            const bool                 hasText = rerevved::nation_select_text::TryEvaluateEraText(
                CIVILIZATION_MONGOLIAN, UNLOCK_ERA_ANCIENT, native, presentation);
            UNSCOPED_INFO("selection text follows live OCC x mod matrix before freeze");
            REQUIRE(hasText == (enabled || modEnabled));
            UNSCOPED_INFO("selection uses the Horseback description");
            REQUIRE((!hasText || std::string_view(presentation.text) == "Start with Horseback Riding"));
            UNSCOPED_INFO("selection does not freeze the pending checkbox");
            REQUIRE(occ::ForGame() == previousGame);

            FreezeOccForGame();
            const auto game = occ::ForGame();
            REXCVAR_SET(one_city_challenge, !enabled);
            const auto  expected = enabled || modEnabled ? horseback : native;
            PPCRegister offset{}, era{}, ability{}, civilization{};
            offset.u64  = CIVILIZATION_MONGOLIAN * 16u;
            era.s64     = UNLOCK_ERA_ANCIENT;
            ability.s64 = native;
            ApplyUniqueEraAbilityCell(offset, era, ability);
            UNSCOPED_INFO("ability follows frozen OCC x mod matrix despite pending checkbox");
            REQUIRE(ability.s64 == expected);
            ApplyUniqueEraAbilityCell(offset, era, ability);
            UNSCOPED_INFO("repeated consumption retains one effective ability");
            REQUIRE(ability.s64 == expected);
            civilization.s64 = CIVILIZATION_MONGOLIAN;
            ApplyBarbarianVillageCityReplacement(civilization);
            UNSCOPED_INFO("camp conversion follows the same replacement matrix");
            REQUIRE(civilization.s64 == (enabled || modEnabled ? CIVILIZATION_UNKNOWN : CIVILIZATION_MONGOLIAN));
            UNSCOPED_INFO("both ability consumers retain the frozen policy");
            REQUIRE(occ::ForGame() == game);

            EraAbilityCellEvaluation published{};
            UNSCOPED_INFO("published ability evaluates");
            REQUIRE(rerevved::unique_era_abilities::TryEvaluate(CIVILIZATION_MONGOLIAN, UNLOCK_ERA_ANCIENT, native, published));
            UNSCOPED_INFO("OCC leaves the published evaluation unchanged");
            REQUIRE((published.effectiveAbility == (modEnabled ? horseback : native) && published.replacementCount == static_cast<uint32_t>(modEnabled)));
            UNSCOPED_INFO("OCC introduces no provider conflict");
            REQUIRE(published.statusFlags == (modEnabled ? ERA_ABILITY_EVALUATION_REPLACED : 0u));
            uint32_t abilityCount = 99;
            uint32_t textCount    = 99;
            UNSCOPED_INFO("OCC publishes no additional ability or grant");
            REQUIRE((GetEraAbilityRuleCount(&abilityCount) == ERA_ABILITIES_OK && abilityCount == static_cast<uint32_t>(modEnabled)));
            UNSCOPED_INFO("OCC publishes no additional text rule");
            REQUIRE((GetNationSelectTextRuleCount(&textCount) == NATION_SELECT_TEXT_OK && textCount == static_cast<uint32_t>(modEnabled)));
        }
    }
    rerevved::unique_era_abilities::ResetForTests();
    rerevved::nation_select_text::ResetForTests();
    REXCVAR_SET(one_city_challenge, false);
    FreezeOccForGame();
}

void testMongolHorsebackSelectors()
{
    const occ::Policy enabled{ true, 33 };
    const occ::Policy disabled{ false, 33 };
    REXCVAR_SET(one_city_challenge, true);
    for (int32_t civilization = 0; civilization < CIVILIZATION_COUNT; ++civilization)
    {
        for (int32_t era = UNLOCK_ERA_ANCIENT; era <= UNLOCK_ERA_MODERN; ++era)
        {
            const bool         target   = civilization == CIVILIZATION_MONGOLIAN && era == UNLOCK_ERA_ANCIENT;
            const EraAbilityId expected = target ? EraAbilityId{ ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING } : EraAbilityId{ ERA_ABILITY_POTTERY };
            UNSCOPED_INFO("override changes only Mongolian Ancient");
            REQUIRE(occ::EffectiveEraAbility(enabled, civilization, era, ERA_ABILITY_POTTERY) == expected);
            UNSCOPED_INFO("off preserves other published replacements");
            REQUIRE(occ::EffectiveEraAbility(disabled, civilization, era, ERA_ABILITY_POTTERY) == ERA_ABILITY_POTTERY);
            UNSCOPED_INFO("live text changes only Mongolian Ancient");
            REQUIRE((occ::NationSelectEraText(civilization, era) != nullptr) == target);
        }
    }
    REXCVAR_SET(one_city_challenge, false);
}

void testThresholds()
{
    const occ::Policy enabled{ true, 33 };
    const occ::Policy disabled{ false, 33 };
    for (int32_t type = 0; type < 38; ++type)
    {
        for (int32_t native : { 2, 3, 40 })
        {
            const auto candidate  = occ::CandidateThreshold(enabled, type, native);
            const auto completion = occ::CompletionThreshold(enabled, type, native - 1);
            UNSCOPED_INFO("off candidate is native");
            REQUIRE(occ::CandidateThreshold(disabled, type, native) == native);
            UNSCOPED_INFO("off completion is native");
            REQUIRE(occ::CompletionThreshold(disabled, type, native - 1) == native - 1);
            for (int32_t population = -128; population <= 127; ++population)
            {
                const bool accepted = population >= native && (type != 0 || population >= 33);
                UNSCOPED_INFO("candidate rejects Settlers below 33 only");
                REQUIRE((population >= candidate) == accepted);
                UNSCOPED_INFO("completion strict comparison uses R-1");
                REQUIRE((population > completion) == accepted);
            }
        }
    }
    REXCVAR_SET(one_city_challenge, false);
    FreezeOccForGame();
    PPCRegister threshold{}, type{}, population{};
    threshold.u64  = 0x1234567800000002ull;
    type.u64       = 0x8765432100000000ull;
    population.u64 = 0xABCDEF000000001Full;
    ApplyOccCandidateThreshold(threshold, type, population);
    ApplyOccCompletionThreshold(threshold, type, population);
    UNSCOPED_INFO("off hook retains every threshold bit");
    REQUIRE(threshold.u64 == 0x1234567800000002ull);
    UNSCOPED_INFO("hook inputs retained");
    REQUIRE((type.u64 == 0x8765432100000000ull && population.u64 == 0xABCDEF000000001Full));
    UNSCOPED_INFO("off AI guard reads no guest stack");
    REQUIRE(!GuardOccAiChoice(type, threshold));
    UNSCOPED_INFO("off reward reads no guest memory");
    REQUIRE(!ApplyOccGoldReward(type, population));
    REXCVAR_SET(one_city_challenge, true);
    FreezeOccForGame();
    ApplyOccCandidateThreshold(threshold, type, population);
    UNSCOPED_INFO("candidate hook writes the full signed operand");
    REQUIRE(threshold.s64 == 33);
    threshold.s64 = 1;
    ApplyOccCompletionThreshold(threshold, type, population);
    UNSCOPED_INFO("completion hook writes R-1");
    REQUIRE(threshold.s64 == 32);
    type.s64      = 7;
    threshold.u64 = 0x1234567800000002ull;
    ApplyOccCandidateThreshold(threshold, type, population);
    ApplyOccCompletionThreshold(threshold, type, population);
    UNSCOPED_INFO("non-Settlers hook retains every operand bit");
    REQUIRE(threshold.u64 == 0x1234567800000002ull);
    population.u64 = 0;
    UNSCOPED_INFO("preview skips the Settler branch without accessing cities");
    REQUIRE(ApplyOccGoldReward(type, population));
}

void testAiEmptyRange()
{
    const occ::Policy enabled{ true, 33 };
    const occ::Policy disabled{ false, 33 };
    for (int32_t prefix = 0; prefix <= 100; ++prefix)
    {
        UNSCOPED_INFO("empty suffix returns no choice even at allocation end");
        REQUIRE(occ::RejectAiChoice(enabled, prefix, prefix, prefix));
        UNSCOPED_INFO("off fallback remains native");
        REQUIRE(!occ::RejectAiChoice(disabled, prefix, prefix, prefix));
        for (int32_t total = prefix + 1; total <= 100; ++total)
        {
            UNSCOPED_INFO("first populated candidate is valid");
            REQUIRE(!occ::RejectAiChoice(enabled, prefix, total, prefix));
            UNSCOPED_INFO("last populated candidate is valid");
            REQUIRE(!occ::RejectAiChoice(enabled, prefix, total, total - 1));
            UNSCOPED_INFO("one-past ordinal is rejected before reading");
            REQUIRE(occ::RejectAiChoice(enabled, prefix, total, total));
        }
    }
    UNSCOPED_INFO("inverted range cannot index storage");
    REQUIRE(occ::RejectAiChoice(enabled, 2, 1, 2));
    UNSCOPED_INFO("invalid prefix cannot index storage");
    REQUIRE(occ::RejectAiChoice(enabled, -1, 10, 0));
    UNSCOPED_INFO("range cannot exceed the allocation");
    REQUIRE(occ::RejectAiChoice(enabled, 0, 101, 0));
}

void testStandardRules()
{
    REXCVAR_SET(one_city_challenge, true);
    occ::FreezeForGame(true);
    const auto standard = occ::ForGame();
    UNSCOPED_INFO("standard rules disable OCC despite a checked live setting");
    REQUIRE(!standard->enabled);
    UNSCOPED_INFO("standard rules retain native Settlers requirements");
    REQUIRE(occ::CandidateThreshold(*standard, occ::kSettlers, 2) == 2);
    UNSCOPED_INFO("standard rules retain the native Mongol ability");
    REQUIRE(occ::EffectiveEraAbility(*standard, CIVILIZATION_MONGOLIAN, UNLOCK_ERA_ANCIENT, ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES) == ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES);
    REXCVAR_SET(one_city_challenge, false);
    UNSCOPED_INFO("pending config changes cannot alter the frozen standard policy");
    REQUIRE(!occ::ForGame()->enabled);
    REXCVAR_SET(one_city_challenge, true);
    occ::FreezeForGame(false);
    UNSCOPED_INFO("unchecked Standard Rules uses the player's OCC selection");
    REQUIRE(occ::ForGame()->enabled);
}

void testOwnedCityReward()
{
    const occ::Policy enabled{ true, 33 };
    const occ::Policy disabled{ false, 33 };
    Cities            cities;
    cities.records[0]   = { 2, 1 };
    cities.records[1]   = { 2, 30 };
    cities.records[2]   = { 2, 31 };
    cities.records[3]   = { 1, 9 };
    cities.records[4]   = { 2, 0 };
    cities.records[127] = { 2, 12 };
    UNSCOPED_INFO("off reward has no city effects");
    REQUIRE((!occ::ApplyGoldReward(disabled, 2, true, cities) && cities.reads == 0));
    UNSCOPED_INFO("preview has no city effects");
    REQUIRE((occ::ApplyGoldReward(enabled, 2, false, cities) && cities.reads == 0));
    UNSCOPED_INFO("enabled reward replaces the native Settler grant");
    REQUIRE(occ::ApplyGoldReward(enabled, 2, true, cities));
    UNSCOPED_INFO("helper runs once for every growing owned city including last slot");
    REQUIRE(cities.refreshed == std::vector<int32_t>{ 0, 1, 127 });
    UNSCOPED_INFO("diagnostics include the cap skip");
    REQUIRE(cities.observed == std::vector<int32_t>{ 0, 1, 2, 127 });
    UNSCOPED_INFO("all owned cities gain population");
    REQUIRE((cities.records[0].population == 2 && cities.records[1].population == 31 && cities.records[127].population == 13));
    UNSCOPED_INFO("cap, other owners and empty cities are retained");
    REQUIRE((cities.records[2].population == 31 && cities.records[3].population == 9 && cities.records[4].population == 0));
    Cities noCities;
    UNSCOPED_INFO("no-city reward retains the native marker continuation");
    REQUIRE((occ::ApplyGoldReward(enabled, 2, true, noCities) && noCities.refreshed.empty()));
}

} // namespace

TEST_CASE("occ_settings", "[ctest][occ-settings][local-fast]")
{
    REREVVED_RESTORE_CVAR(one_city_challenge);
    const PolicyRestore                        policyRestore;
    const rerevved::tests::ScopedRegistryReset registryScope;
    testFrozenPolicy();
    testMongolHorsebackMatrix();
    testMongolHorsebackSelectors();
    testThresholds();
    testAiEmptyRange();
    testOwnedCityReward();
    testStandardRules();
}
