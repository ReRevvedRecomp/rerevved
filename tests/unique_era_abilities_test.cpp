#include "registry_scope.hpp"
#include "unique_era_abilities_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <rex/ppc.h>

extern "C" int rerevved_unique_era_abilities_c_header_check(void);

void ApplyUniqueEraAbilityCell(PPCRegister& cellOffset,
                               PPCRegister& unlockEra,
                               PPCRegister& nativeAbility);
void ApplyBarbarianVillageCityReplacement(
    PPCRegister& civilization);
void BeginHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase);
void EndHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase);
void SelectHorsebackRidingAbility(PPCRegister& ability);
void SelectHorsebackRidingTechnology(PPCRegister& technology);

namespace
{

EraAbilityReplacement makeRule(
    const char*    provider,
    const char*    ruleId,
    CivilizationId civilization,
    UnlockEra      unlockEra,
    EraAbilityId   replacement)
{
    EraAbilityReplacement rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization       = civilization;
    rule.unlockEra          = unlockEra;
    rule.replacementAbility = replacement;
    return rule;
}

EraAbilityCellEvaluation evaluate(
    CivilizationId civilization,
    UnlockEra      unlockEra,
    EraAbilityId   nativeAbility)
{
    const EraAbilityCellQuery query = {
        sizeof(EraAbilityCellQuery),
        civilization,
        unlockEra,
        nativeAbility,
        {},
    };
    EraAbilityCellEvaluation result{};
    UNSCOPED_INFO("cell evaluation failed");
    REQUIRE(EvaluateEraAbilityCell(
                &query, &result, sizeof(result)) ==
            ERA_ABILITIES_OK);
    return result;
}

void TestAbiLayout()
{
    static_assert(offsetof(EraAbilityReplacement, providerId) ==
                  4);
    static_assert(offsetof(EraAbilityReplacement, ruleId) == 68);
    static_assert(
        offsetof(EraAbilityReplacement, civilization) == 132);
    static_assert(
        offsetof(EraAbilityReplacement, replacementAbility) ==
        140);
    static_assert(
        offsetof(EraAbilityRuleInfo, statusFlags) == 144);
    static_assert(offsetof(EraAbilityCellEvaluation,
                           effectiveAbility) == 8);
    UNSCOPED_INFO("ABI version mismatch");
    REQUIRE(EraAbilitiesAbiVersion() ==
            ERA_ABILITIES_ABI_VERSION);
}

void TestValidation()
{
    rerevved::unique_era_abilities::ResetForTests();
    UNSCOPED_INFO("null rule accepted");
    REQUIRE(RegisterEraAbilityReplacement(nullptr) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);

    auto rule = makeRule("aeshur.roman-rush-test",
                         "roman-medieval-rush",
                         CIVILIZATION_ROMAN,
                         UNLOCK_ERA_MEDIEVAL,
                         ERA_ABILITY_UNIT_RUSH_HALF_COST);
    rule.structSize--;
    UNSCOPED_INFO("short rule accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    rule.structSize++;

    rule.providerId[0] = 'A';
    UNSCOPED_INFO("malformed provider accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    constexpr char provider[] = "aeshur.roman-rush-test";
    std::memcpy(rule.providerId, provider, sizeof(provider));
    rule.civilization = CIVILIZATION_UNKNOWN;
    UNSCOPED_INFO("unknown civilization accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    rule.civilization = CIVILIZATION_ROMAN;
    rule.unlockEra    = 4;
    UNSCOPED_INFO("unknown era accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    rule.unlockEra          = UNLOCK_ERA_MEDIEVAL;
    rule.replacementAbility = 11;
    UNSCOPED_INFO("unsupported UEA accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    rule.replacementAbility =
        ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    UNSCOPED_INFO("title-owned synthetic UEA rejected");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_OK);
    rerevved::unique_era_abilities::ResetForTests();
    rule.replacementAbility =
        ERA_ABILITY_UNIT_RUSH_HALF_COST;
    rule.reserved[7] = 1;
    UNSCOPED_INFO("nonzero reserved field accepted");
    REQUIRE(RegisterEraAbilityReplacement(&rule) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);

    uint32_t count = 0;
    UNSCOPED_INFO("null count accepted");
    REQUIRE(GetEraAbilityRuleCount(nullptr) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("invalid registration mutated registry");
    REQUIRE((GetEraAbilityRuleCount(&count) ==
                 ERA_ABILITIES_OK &&
             count == 0));
}

void TestAcceptedSemanticRegistry()
{
    constexpr std::array<EraAbilityId, 45> abilities = {
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        12,
        13,
        14,
        16,
        17,
        18,
        19,
        20,
        23,
        24,
        25,
        26,
        27,
        28,
        30,
        32,
        34,
        35,
        36,
        38,
        40,
        41,
        42,
        43,
        46,
        47,
        48,
        50,
        51,
        55,
        56,
        58,
        59,
        60,
        61,
    };
    rerevved::unique_era_abilities::ResetForTests();
    for (int32_t ability = 0; ability <= 62; ++ability)
    {
        EraAbilityCellQuery query{};
        query.structSize    = sizeof(query);
        query.civilization  = CIVILIZATION_ROMAN;
        query.unlockEra     = UNLOCK_ERA_ANCIENT;
        query.nativeAbility = ability;
        EraAbilityCellEvaluation result{};
        const bool               expected =
            std::find(abilities.begin(), abilities.end(), ability) !=
            abilities.end();
        UNSCOPED_INFO("semantic registry accepted the wrong numeric UEA set");
        REQUIRE((EvaluateEraAbilityCell(
                     &query, &result, sizeof(result)) ==
                 ERA_ABILITIES_OK) == expected);
        if (expected)
        {
            UNSCOPED_INFO("accepted no-rule ability changed");
            REQUIRE((result.nativeAbility == ability &&
                     result.effectiveAbility == ability &&
                     result.replacementCount == 0 &&
                     result.statusFlags == 0));
        }
    }
    EraAbilityCellQuery syntheticQuery{};
    syntheticQuery.structSize   = sizeof(syntheticQuery);
    syntheticQuery.civilization = CIVILIZATION_MONGOLIAN;
    syntheticQuery.unlockEra    = UNLOCK_ERA_ANCIENT;
    syntheticQuery.nativeAbility =
        ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    EraAbilityCellEvaluation syntheticResult{};
    UNSCOPED_INFO("synthetic UEA was accepted as a native table value");
    REQUIRE(EvaluateEraAbilityCell(&syntheticQuery,
                                   &syntheticResult,
                                   sizeof(syntheticResult)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    // Cell validation is independent of the accepted ability values above.
    for (int32_t civilization = CIVILIZATION_ROMAN;
         civilization < CIVILIZATION_COUNT;
         ++civilization)
    {
        for (int32_t era = UNLOCK_ERA_ANCIENT;
             era <= UNLOCK_ERA_MODERN;
             ++era)
        {
            const auto result = evaluate(civilization, era, abilities.front());
            UNSCOPED_INFO("accepted no-rule cell changed");
            REQUIRE((result.nativeAbility == abilities.front() &&
                     result.effectiveAbility == abilities.front() &&
                     result.replacementCount == 0 &&
                     result.statusFlags == 0));
        }
    }
}

void TestRegistrationAndReadback()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto roman   = makeRule("z.provider",
                            "roman-medieval-rush",
                            CIVILIZATION_ROMAN,
                            UNLOCK_ERA_MEDIEVAL,
                            ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english = makeRule("a.provider",
                            "english-ancient-rush",
                            CIVILIZATION_ENGLISH,
                            UNLOCK_ERA_ANCIENT,
                            ERA_ABILITY_UNIT_RUSH_HALF_COST);
    UNSCOPED_INFO("valid rules rejected");
    REQUIRE((RegisterEraAbilityReplacement(&roman) ==
                 ERA_ABILITIES_OK &&
             RegisterEraAbilityReplacement(&english) ==
                 ERA_ABILITIES_OK));

    std::memset(roman.providerId + std::strlen(roman.providerId) + 1,
                'x',
                sizeof(roman.providerId) - std::strlen(roman.providerId) - 1);
    UNSCOPED_INFO("normalized duplicate was not idempotent");
    REQUIRE(RegisterEraAbilityReplacement(&roman) ==
            ERA_ABILITIES_OK);
    roman.replacementAbility = ERA_ABILITY_WONDERS_HALF_COST;
    UNSCOPED_INFO("changed duplicate key accepted");
    REQUIRE(RegisterEraAbilityReplacement(&roman) ==
            ERA_ABILITIES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("wrong rule count");
    REQUIRE((GetEraAbilityRuleCount(&count) ==
                 ERA_ABILITIES_OK &&
             count == 2));
    EraAbilityRuleInfo info{};
    UNSCOPED_INFO("readback is not canonical");
    REQUIRE((GetEraAbilityRule(0, &info, sizeof(info)) ==
                 ERA_ABILITIES_OK &&
             std::strcmp(info.providerId, "a.provider") == 0));
    UNSCOPED_INFO("short readback buffer accepted");
    REQUIRE(GetEraAbilityRule(0, &info, 147) ==
            ERA_ABILITIES_ERR_BUFFER_TOO_SMALL);
    UNSCOPED_INFO("minimum readback prefix rejected");
    REQUIRE((GetEraAbilityRule(0, &info, 148) ==
                 ERA_ABILITIES_OK &&
             info.structSize == sizeof(info)));
}

void TestCompositionAndConflict()
{
    auto roman    = makeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             CIVILIZATION_ROMAN,
                             UNLOCK_ERA_MEDIEVAL,
                             ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english  = makeRule("example.english",
                             "english-modern-rush",
                             CIVILIZATION_ENGLISH,
                             UNLOCK_ERA_MODERN,
                             ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto conflict = makeRule("example.conflict",
                             "roman-medieval-pottery",
                             CIVILIZATION_ROMAN,
                             UNLOCK_ERA_MEDIEVAL,
                             ERA_ABILITY_POTTERY);

    rerevved::unique_era_abilities::ResetForTests();
    UNSCOPED_INFO("distinct cells failed registration");
    REQUIRE((RegisterEraAbilityReplacement(&english) == 0 &&
             RegisterEraAbilityReplacement(&roman) == 0));
    auto romanResult   = evaluate(CIVILIZATION_ROMAN,
                                  UNLOCK_ERA_MEDIEVAL,
                                  ERA_ABILITY_WONDERS_HALF_COST);
    auto englishResult = evaluate(
        CIVILIZATION_ENGLISH,
        UNLOCK_ERA_MODERN,
        ERA_ABILITY_DOUBLE_NAVAL_SUPPORT);
    UNSCOPED_INFO("distinct cells did not compose");
    REQUIRE((romanResult.effectiveAbility ==
                 ERA_ABILITY_UNIT_RUSH_HALF_COST &&
             englishResult.effectiveAbility ==
                 ERA_ABILITY_UNIT_RUSH_HALF_COST));

    UNSCOPED_INFO("conflicting replacement registration failed");
    REQUIRE(RegisterEraAbilityReplacement(&conflict) == 0);
    romanResult = evaluate(CIVILIZATION_ROMAN,
                           UNLOCK_ERA_MEDIEVAL,
                           ERA_ABILITY_WONDERS_HALF_COST);
    UNSCOPED_INFO("same-cell conflict did not restore native ability");
    REQUIRE((romanResult.effectiveAbility ==
                 ERA_ABILITY_WONDERS_HALF_COST &&
             romanResult.replacementCount == 2 &&
             romanResult.statusFlags ==
                 ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT));

    for (uint32_t index = 0; index < 3; ++index)
    {
        EraAbilityRuleInfo info{};
        UNSCOPED_INFO("conflict readback failed");
        REQUIRE(GetEraAbilityRule(index, &info, sizeof(info)) == 0);
        const bool romanCell =
            info.civilization == CIVILIZATION_ROMAN &&
            info.unlockEra == UNLOCK_ERA_MEDIEVAL;
        UNSCOPED_INFO("conflict was not disclosed on readback");
        REQUIRE((!romanCell ||
                 info.statusFlags ==
                     ERA_ABILITY_RULE_REPLACEMENT_CONFLICT));
    }

    rerevved::unique_era_abilities::ResetForTests();
    UNSCOPED_INFO("reverse conflict order failed registration");
    REQUIRE((RegisterEraAbilityReplacement(&conflict) == 0 &&
             RegisterEraAbilityReplacement(&roman) == 0));
    romanResult = evaluate(CIVILIZATION_ROMAN,
                           UNLOCK_ERA_MEDIEVAL,
                           ERA_ABILITY_WONDERS_HALF_COST);
    UNSCOPED_INFO("conflict depended on registration order");
    REQUIRE((romanResult.effectiveAbility ==
                 ERA_ABILITY_WONDERS_HALF_COST &&
             romanResult.replacementCount == 2));
}

void TestDuplicateEffectiveAbilityAndBridge()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto ancient  = makeRule("example.roman",
                             "roman-ancient-rush",
                             CIVILIZATION_ROMAN,
                             UNLOCK_ERA_ANCIENT,
                             ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto medieval = makeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             CIVILIZATION_ROMAN,
                             UNLOCK_ERA_MEDIEVAL,
                             ERA_ABILITY_UNIT_RUSH_HALF_COST);
    UNSCOPED_INFO("duplicate-effective cells failed registration");
    REQUIRE((RegisterEraAbilityReplacement(&ancient) == 0 &&
             RegisterEraAbilityReplacement(&medieval) == 0));
    UNSCOPED_INFO("duplicate effective UEA was suppressed");
    REQUIRE((evaluate(CIVILIZATION_ROMAN,
                      UNLOCK_ERA_ANCIENT,
                      ERA_ABILITY_ROADS_HALF_COST)
                     .effectiveAbility ==
                 ERA_ABILITY_UNIT_RUSH_HALF_COST &&
             evaluate(CIVILIZATION_ROMAN,
                      UNLOCK_ERA_MEDIEVAL,
                      ERA_ABILITY_WONDERS_HALF_COST)
                     .effectiveAbility ==
                 ERA_ABILITY_UNIT_RUSH_HALF_COST));

    PPCRegister offset{};
    PPCRegister era{};
    PPCRegister ability{};
    offset.u64  = 4;
    era.s64     = UNLOCK_ERA_MEDIEVAL;
    ability.s64 = ERA_ABILITY_WONDERS_HALF_COST;
    ApplyUniqueEraAbilityCell(offset, era, ability);
    UNSCOPED_INFO("Roman Medieval bridge did not apply 24 -> 35");
    REQUIRE(ability.s32 ==
            ERA_ABILITY_UNIT_RUSH_HALF_COST);

    offset.u64  = 6;
    ability.s64 = ERA_ABILITY_WONDERS_HALF_COST;
    ApplyUniqueEraAbilityCell(offset, era, ability);
    UNSCOPED_INFO("unaligned bridge offset changed native value");
    REQUIRE(ability.s32 == ERA_ABILITY_WONDERS_HALF_COST);

    offset.u64 = 4;
    era.s64    = 4;
    ApplyUniqueEraAbilityCell(offset, era, ability);
    UNSCOPED_INFO("invalid bridge era changed native value");
    REQUIRE(ability.s32 == ERA_ABILITY_WONDERS_HALF_COST);
}

void TestHorsebackRidingReplacement()
{
    rerevved::unique_era_abilities::ResetForTests();

    PPCRegister nativeMongolian{};
    nativeMongolian.s64 = CIVILIZATION_MONGOLIAN;
    ApplyBarbarianVillageCityReplacement(nativeMongolian);
    UNSCOPED_INFO("native Mongolian village conversion was suppressed without a rule");
    REQUIRE(nativeMongolian.s32 == CIVILIZATION_MONGOLIAN);

    auto rule = makeRule(
        "aeshur.mongol-horseback-riding",
        "mongol-ancient-horseback-riding",
        CIVILIZATION_MONGOLIAN,
        UNLOCK_ERA_ANCIENT,
        ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING);
    UNSCOPED_INFO("Horseback Riding replacement registration failed");
    REQUIRE(RegisterEraAbilityReplacement(&rule) == 0);

    const auto result = evaluate(
        CIVILIZATION_MONGOLIAN,
        UNLOCK_ERA_ANCIENT,
        ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES);
    UNSCOPED_INFO("Mongolian Ancient replacement did not produce Horseback Riding");
    REQUIRE((result.nativeAbility ==
                 ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES &&
             result.effectiveAbility ==
                 ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING &&
             result.replacementCount == 1 &&
             result.statusFlags ==
                 ERA_ABILITY_EVALUATION_REPLACED));

    PPCRegister replacedMongolian{};
    replacedMongolian.s64 = CIVILIZATION_MONGOLIAN;
    ApplyBarbarianVillageCityReplacement(replacedMongolian);
    UNSCOPED_INFO("Horseback Riding replacement did not suppress village conversion");
    REQUIRE(replacedMongolian.s32 == CIVILIZATION_UNKNOWN);

    PPCRegister nonMongolian{};
    nonMongolian.s64 = CIVILIZATION_ROMAN;
    ApplyBarbarianVillageCityReplacement(nonMongolian);
    UNSCOPED_INFO("village gate changed a non-Mongolian civilization");
    REQUIRE(nonMongolian.s32 == CIVILIZATION_ROMAN);

    auto conflict = makeRule(
        "example.conflicting-provider",
        "mongol-ancient-conflict",
        CIVILIZATION_MONGOLIAN,
        UNLOCK_ERA_ANCIENT,
        ERA_ABILITY_UNIT_RUSH_HALF_COST);
    UNSCOPED_INFO("Mongolian Ancient conflict registration failed");
    REQUIRE(RegisterEraAbilityReplacement(&conflict) == 0);
    PPCRegister conflictedMongolian{};
    conflictedMongolian.s64 = CIVILIZATION_MONGOLIAN;
    ApplyBarbarianVillageCityReplacement(conflictedMongolian);
    UNSCOPED_INFO("same-cell conflict did not preserve native village conversion");
    REQUIRE(conflictedMongolian.s32 == CIVILIZATION_MONGOLIAN);

    PPCRegister ownershipBase{};
    ownershipBase.u64 = 0x1000;
    BeginHorsebackRidingOwnershipCheck(ownershipBase);
    UNSCOPED_INFO("Horseback Riding ownership check did not select technology 4");
    REQUIRE(ownershipBase.u32 + 72 == 0x1010);
    EndHorsebackRidingOwnershipCheck(ownershipBase);
    UNSCOPED_INFO("Horseback Riding ownership check did not restore its base");
    REQUIRE(ownershipBase.u32 == 0x1000);

    PPCRegister ability{};
    PPCRegister technology{};
    SelectHorsebackRidingAbility(ability);
    SelectHorsebackRidingTechnology(technology);
    UNSCOPED_INFO("Horseback Riding consumer selected the wrong IDs");
    REQUIRE((ability.s32 ==
                 ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING &&
             technology.s32 == 4));
}

void TestQueryErrors()
{
    rerevved::unique_era_abilities::ResetForTests();
    EraAbilityCellQuery query{};
    query.structSize    = sizeof(query);
    query.civilization  = CIVILIZATION_ROMAN;
    query.unlockEra     = UNLOCK_ERA_MEDIEVAL;
    query.nativeAbility = ERA_ABILITY_WONDERS_HALF_COST;
    EraAbilityCellEvaluation out{};
    UNSCOPED_INFO("null output accepted");
    REQUIRE(EvaluateEraAbilityCell(
                &query, nullptr, sizeof(out)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("short output buffer accepted");
    REQUIRE(EvaluateEraAbilityCell(&query, &out, 19) ==
            ERA_ABILITIES_ERR_BUFFER_TOO_SMALL);
    UNSCOPED_INFO("minimum output prefix rejected");
    REQUIRE(EvaluateEraAbilityCell(&query, &out, 20) ==
            ERA_ABILITIES_OK);
    query.nativeAbility = 11;
    UNSCOPED_INFO("non-retail native UEA accepted");
    REQUIRE(EvaluateEraAbilityCell(
                &query, &out, sizeof(out)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    query.nativeAbility = ERA_ABILITY_WONDERS_HALF_COST;
    query.reserved[0]   = 1;
    UNSCOPED_INFO("nonzero query reserve accepted");
    REQUIRE(EvaluateEraAbilityCell(
                &query, &out, sizeof(out)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    query.reserved[0]  = 0;
    query.civilization = CIVILIZATION_COUNT;
    UNSCOPED_INFO("upper civilization bound accepted");
    REQUIRE(EvaluateEraAbilityCell(
                &query, &out, sizeof(out)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
    query.civilization = CIVILIZATION_ROMAN;
    query.unlockEra    = -1;
    UNSCOPED_INFO("lower era bound accepted");
    REQUIRE(EvaluateEraAbilityCell(
                &query, &out, sizeof(out)) ==
            ERA_ABILITIES_ERR_INVALID_ARGUMENT);
}

} // namespace

TEST_CASE("unique_era_abilities_contract", "[ctest][unique-era-abilities][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestAbiLayout();
    TestValidation();
    TestAcceptedSemanticRegistry();
    TestRegistrationAndReadback();
    TestCompositionAndConflict();
    TestDuplicateEffectiveAbilityAndBridge();
    TestHorsebackRidingReplacement();
    TestQueryErrors();
}

TEST_CASE("unique_era_abilities_c_header", "[ctest][unique-era-abilities][local-fast]")
{
    const int status = rerevved_unique_era_abilities_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
