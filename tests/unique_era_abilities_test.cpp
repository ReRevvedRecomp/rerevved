#include "unique_era_abilities_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

#include <rex/ppc.h>

void ReRevvedApplyUniqueEraAbilityCell(PPCRegister& cellOffset,
                                       PPCRegister& unlockEra,
                                       PPCRegister& nativeAbility);
void ReRevvedApplyBarbarianVillageCityReplacement(
    PPCRegister& civilization);
void ReRevvedBeginHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase);
void ReRevvedEndHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase);
void ReRevvedSelectHorsebackRidingAbility(PPCRegister& ability);
void ReRevvedSelectHorsebackRidingTechnology(PPCRegister& technology);

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedUniqueEraAbilityReplacement makeRule(
    const char*                provider,
    const char*                ruleId,
    ReRevvedCivilizationId     civilization,
    ReRevvedUniqueEraUnlockEra unlockEra,
    ReRevvedUniqueEraAbilityId replacement)
{
    ReRevvedUniqueEraAbilityReplacement rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization       = civilization;
    rule.unlockEra          = unlockEra;
    rule.replacementAbility = replacement;
    return rule;
}

ReRevvedUniqueEraAbilityCellEvaluation evaluate(
    ReRevvedCivilizationId     civilization,
    ReRevvedUniqueEraUnlockEra unlockEra,
    ReRevvedUniqueEraAbilityId nativeAbility)
{
    const ReRevvedUniqueEraAbilityCellQuery query = {
        sizeof(ReRevvedUniqueEraAbilityCellQuery),
        civilization,
        unlockEra,
        nativeAbility,
        {},
    };
    ReRevvedUniqueEraAbilityCellEvaluation result{};
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &result, sizeof(result)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "cell evaluation failed");
    return result;
}

void TestAbiLayout()
{
    static_assert(sizeof(ReRevvedUniqueEraAbilityReplacement) == 176);
    static_assert(offsetof(ReRevvedUniqueEraAbilityReplacement, providerId) ==
                  4);
    static_assert(offsetof(ReRevvedUniqueEraAbilityReplacement, ruleId) == 68);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityReplacement, civilization) == 132);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityReplacement, replacementAbility) ==
        140);
    static_assert(sizeof(ReRevvedUniqueEraAbilityRuleInfo) == 180);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityRuleInfo, statusFlags) == 144);
    static_assert(sizeof(ReRevvedUniqueEraAbilityCellQuery) == 40);
    static_assert(sizeof(ReRevvedUniqueEraAbilityCellEvaluation) == 40);
    static_assert(offsetof(ReRevvedUniqueEraAbilityCellEvaluation,
                           effectiveAbility) == 8);
    require(ReRevvedUniqueEraAbilitiesAbiVersion() ==
                REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION,
            "ABI version mismatch");
}

void TestValidation()
{
    rerevved::unique_era_abilities::ResetForTests();
    require(ReRevvedRegisterUniqueEraAbilityReplacement(nullptr) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null rule accepted");

    auto rule = makeRule("aeshur.roman-rush-test",
                         "roman-medieval-rush",
                         REREVVED_CIVILIZATION_ROMAN,
                         REREVVED_UNIQUE_ERA_MEDIEVAL,
                         REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    rule.structSize--;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "short rule accepted");
    rule.structSize++;

    rule.providerId[0] = 'A';
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "malformed provider accepted");
    constexpr char provider[] = "aeshur.roman-rush-test";
    std::memcpy(rule.providerId, provider, sizeof(provider));
    rule.civilization = REREVVED_CIVILIZATION_UNKNOWN;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "unknown civilization accepted");
    rule.civilization = REREVVED_CIVILIZATION_ROMAN;
    rule.unlockEra    = 4;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "unknown era accepted");
    rule.unlockEra          = REREVVED_UNIQUE_ERA_MEDIEVAL;
    rule.replacementAbility = 11;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "unsupported UEA accepted");
    rule.replacementAbility =
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "title-owned synthetic UEA rejected");
    rerevved::unique_era_abilities::ResetForTests();
    rule.replacementAbility =
        REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST;
    rule.reserved[7] = 1;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field accepted");

    uint32_t count = 0;
    require(ReRevvedGetUniqueEraAbilityRuleCount(nullptr) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null count accepted");
    require(ReRevvedGetUniqueEraAbilityRuleCount(&count) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                count == 0,
            "invalid registration mutated registry");
}

void TestAcceptedSemanticRegistry()
{
    constexpr std::array<ReRevvedUniqueEraAbilityId, 45> abilities = {
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
        ReRevvedUniqueEraAbilityCellQuery query{};
        query.structSize    = sizeof(query);
        query.civilization  = REREVVED_CIVILIZATION_ROMAN;
        query.unlockEra     = REREVVED_UNIQUE_ERA_ANCIENT;
        query.nativeAbility = ability;
        ReRevvedUniqueEraAbilityCellEvaluation result{};
        const bool                             expected =
            std::find(abilities.begin(), abilities.end(), ability) !=
            abilities.end();
        require((ReRevvedEvaluateUniqueEraAbilityCell(
                     &query, &result, sizeof(result)) ==
                 REREVVED_UNIQUE_ERA_ABILITIES_OK) == expected,
                "semantic registry accepted the wrong numeric UEA set");
    }
    ReRevvedUniqueEraAbilityCellQuery syntheticQuery{};
    syntheticQuery.structSize   = sizeof(syntheticQuery);
    syntheticQuery.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    syntheticQuery.unlockEra    = REREVVED_UNIQUE_ERA_ANCIENT;
    syntheticQuery.nativeAbility =
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    ReRevvedUniqueEraAbilityCellEvaluation syntheticResult{};
    require(ReRevvedEvaluateUniqueEraAbilityCell(&syntheticQuery,
                                                 &syntheticResult,
                                                 sizeof(syntheticResult)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "synthetic UEA was accepted as a native table value");
    for (int32_t civilization = REREVVED_CIVILIZATION_ROMAN;
         civilization < REREVVED_CIVILIZATION_COUNT;
         ++civilization)
    {
        for (int32_t era = REREVVED_UNIQUE_ERA_ANCIENT;
             era <= REREVVED_UNIQUE_ERA_MODERN;
             ++era)
        {
            for (const auto ability : abilities)
            {
                const auto result = evaluate(civilization, era, ability);
                require(result.nativeAbility == ability &&
                            result.effectiveAbility == ability &&
                            result.replacementCount == 0 &&
                            result.statusFlags == 0,
                        "accepted no-rule cell changed");
            }
        }
    }
}

void TestRegistrationAndReadback()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto roman   = makeRule("z.provider",
                            "roman-medieval-rush",
                            REREVVED_CIVILIZATION_ROMAN,
                            REREVVED_UNIQUE_ERA_MEDIEVAL,
                            REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english = makeRule("a.provider",
                            "english-ancient-rush",
                            REREVVED_CIVILIZATION_ENGLISH,
                            REREVVED_UNIQUE_ERA_ANCIENT,
                            REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&english) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "valid rules rejected");

    std::memset(roman.providerId + std::strlen(roman.providerId) + 1,
                'x',
                sizeof(roman.providerId) - std::strlen(roman.providerId) - 1);
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "normalized duplicate was not idempotent");
    roman.replacementAbility = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate key accepted");

    uint32_t count = 0;
    require(ReRevvedGetUniqueEraAbilityRuleCount(&count) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                count == 2,
            "wrong rule count");
    ReRevvedUniqueEraAbilityRuleInfo info{};
    require(ReRevvedGetUniqueEraAbilityRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                std::strcmp(info.providerId, "a.provider") == 0,
            "readback is not canonical");
    require(ReRevvedGetUniqueEraAbilityRule(0, &info, 147) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL,
            "short readback buffer accepted");
    require(ReRevvedGetUniqueEraAbilityRule(0, &info, 148) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                info.structSize == sizeof(info),
            "minimum readback prefix rejected");
}

void TestCompositionAndConflict()
{
    auto roman    = makeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english  = makeRule("example.english",
                             "english-modern-rush",
                             REREVVED_CIVILIZATION_ENGLISH,
                             REREVVED_UNIQUE_ERA_MODERN,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto conflict = makeRule("example.conflict",
                             "roman-medieval-pottery",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_POTTERY);

    rerevved::unique_era_abilities::ResetForTests();
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&english) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&roman) == 0,
            "distinct cells failed registration");
    auto romanResult   = evaluate(REREVVED_CIVILIZATION_ROMAN,
                                  REREVVED_UNIQUE_ERA_MEDIEVAL,
                                  REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    auto englishResult = evaluate(
        REREVVED_CIVILIZATION_ENGLISH,
        REREVVED_UNIQUE_ERA_MODERN,
        REREVVED_UNIQUE_ERA_ABILITY_DOUBLE_NAVAL_SUPPORT);
    require(romanResult.effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST &&
                englishResult.effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "distinct cells did not compose");

    require(ReRevvedRegisterUniqueEraAbilityReplacement(&conflict) == 0,
            "conflicting replacement registration failed");
    romanResult = evaluate(REREVVED_CIVILIZATION_ROMAN,
                           REREVVED_UNIQUE_ERA_MEDIEVAL,
                           REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    require(romanResult.effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST &&
                romanResult.replacementCount == 2 &&
                romanResult.statusFlags ==
                    REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT,
            "same-cell conflict did not restore native ability");

    for (uint32_t index = 0; index < 3; ++index)
    {
        ReRevvedUniqueEraAbilityRuleInfo info{};
        require(ReRevvedGetUniqueEraAbilityRule(index, &info, sizeof(info)) == 0,
                "conflict readback failed");
        const bool romanCell =
            info.civilization == REREVVED_CIVILIZATION_ROMAN &&
            info.unlockEra == REREVVED_UNIQUE_ERA_MEDIEVAL;
        require(!romanCell ||
                    info.statusFlags ==
                        REREVVED_UNIQUE_ERA_ABILITY_RULE_REPLACEMENT_CONFLICT,
                "conflict was not disclosed on readback");
    }

    rerevved::unique_era_abilities::ResetForTests();
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&conflict) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&roman) == 0,
            "reverse conflict order failed registration");
    romanResult = evaluate(REREVVED_CIVILIZATION_ROMAN,
                           REREVVED_UNIQUE_ERA_MEDIEVAL,
                           REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    require(romanResult.effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST &&
                romanResult.replacementCount == 2,
            "conflict depended on registration order");
}

void TestDuplicateEffectiveAbilityAndBridge()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto ancient  = makeRule("example.roman",
                             "roman-ancient-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_ANCIENT,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto medieval = makeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&ancient) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&medieval) == 0,
            "duplicate-effective cells failed registration");
    require(evaluate(REREVVED_CIVILIZATION_ROMAN,
                     REREVVED_UNIQUE_ERA_ANCIENT,
                     REREVVED_UNIQUE_ERA_ABILITY_ROADS_HALF_COST)
                        .effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST &&
                evaluate(REREVVED_CIVILIZATION_ROMAN,
                         REREVVED_UNIQUE_ERA_MEDIEVAL,
                         REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST)
                        .effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "duplicate effective UEA was suppressed");

    PPCRegister offset{};
    PPCRegister era{};
    PPCRegister ability{};
    offset.u64  = 4;
    era.s64     = REREVVED_UNIQUE_ERA_MEDIEVAL;
    ability.s64 = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    require(ability.s32 ==
                REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "Roman Medieval bridge did not apply 24 -> 35");

    offset.u64  = 6;
    ability.s64 = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    require(ability.s32 == REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST,
            "unaligned bridge offset changed native value");

    offset.u64 = 4;
    era.s64    = 4;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    require(ability.s32 == REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST,
            "invalid bridge era changed native value");
}

void TestHorsebackRidingReplacement()
{
    rerevved::unique_era_abilities::ResetForTests();

    PPCRegister nativeMongolian{};
    nativeMongolian.s64 = REREVVED_CIVILIZATION_MONGOLIAN;
    ReRevvedApplyBarbarianVillageCityReplacement(nativeMongolian);
    require(nativeMongolian.s32 == REREVVED_CIVILIZATION_MONGOLIAN,
            "native Mongolian village conversion was suppressed without a rule");

    auto rule = makeRule(
        "aeshur.mongol-horseback-riding",
        "mongol-ancient-horseback-riding",
        REREVVED_CIVILIZATION_MONGOLIAN,
        REREVVED_UNIQUE_ERA_ANCIENT,
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING);
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) == 0,
            "Horseback Riding replacement registration failed");

    const auto result = evaluate(
        REREVVED_CIVILIZATION_MONGOLIAN,
        REREVVED_UNIQUE_ERA_ANCIENT,
        REREVVED_UNIQUE_ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES);
    require(result.nativeAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES &&
                result.effectiveAbility ==
                    REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING &&
                result.replacementCount == 1 &&
                result.statusFlags ==
                    REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACED,
            "Mongolian Ancient replacement did not produce Horseback Riding");

    PPCRegister replacedMongolian{};
    replacedMongolian.s64 = REREVVED_CIVILIZATION_MONGOLIAN;
    ReRevvedApplyBarbarianVillageCityReplacement(replacedMongolian);
    require(replacedMongolian.s32 == REREVVED_CIVILIZATION_UNKNOWN,
            "Horseback Riding replacement did not suppress village conversion");

    PPCRegister nonMongolian{};
    nonMongolian.s64 = REREVVED_CIVILIZATION_ROMAN;
    ReRevvedApplyBarbarianVillageCityReplacement(nonMongolian);
    require(nonMongolian.s32 == REREVVED_CIVILIZATION_ROMAN,
            "village gate changed a non-Mongolian civilization");

    auto conflict = makeRule(
        "example.conflicting-provider",
        "mongol-ancient-conflict",
        REREVVED_CIVILIZATION_MONGOLIAN,
        REREVVED_UNIQUE_ERA_ANCIENT,
        REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    require(ReRevvedRegisterUniqueEraAbilityReplacement(&conflict) == 0,
            "Mongolian Ancient conflict registration failed");
    PPCRegister conflictedMongolian{};
    conflictedMongolian.s64 = REREVVED_CIVILIZATION_MONGOLIAN;
    ReRevvedApplyBarbarianVillageCityReplacement(conflictedMongolian);
    require(conflictedMongolian.s32 == REREVVED_CIVILIZATION_MONGOLIAN,
            "same-cell conflict did not preserve native village conversion");

    PPCRegister ownershipBase{};
    ownershipBase.u64 = 0x1000;
    ReRevvedBeginHorsebackRidingOwnershipCheck(ownershipBase);
    require(ownershipBase.u32 + 72 == 0x1010,
            "Horseback Riding ownership check did not select technology 4");
    ReRevvedEndHorsebackRidingOwnershipCheck(ownershipBase);
    require(ownershipBase.u32 == 0x1000,
            "Horseback Riding ownership check did not restore its base");

    PPCRegister ability{};
    PPCRegister technology{};
    ReRevvedSelectHorsebackRidingAbility(ability);
    ReRevvedSelectHorsebackRidingTechnology(technology);
    require(ability.s32 ==
                    REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING &&
                technology.s32 == 4,
            "Horseback Riding consumer selected the wrong IDs");
}

void TestQueryErrors()
{
    rerevved::unique_era_abilities::ResetForTests();
    ReRevvedUniqueEraAbilityCellQuery query{};
    query.structSize    = sizeof(query);
    query.civilization  = REREVVED_CIVILIZATION_ROMAN;
    query.unlockEra     = REREVVED_UNIQUE_ERA_MEDIEVAL;
    query.nativeAbility = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedUniqueEraAbilityCellEvaluation out{};
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, nullptr, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null output accepted");
    require(ReRevvedEvaluateUniqueEraAbilityCell(&query, &out, 19) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL,
            "short output buffer accepted");
    require(ReRevvedEvaluateUniqueEraAbilityCell(&query, &out, 20) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "minimum output prefix rejected");
    query.nativeAbility = 11;
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "non-retail native UEA accepted");
    query.nativeAbility = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    query.reserved[0]   = 1;
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "nonzero query reserve accepted");
    query.reserved[0]  = 0;
    query.civilization = REREVVED_CIVILIZATION_COUNT;
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "upper civilization bound accepted");
    query.civilization = REREVVED_CIVILIZATION_ROMAN;
    query.unlockEra    = -1;
    require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "lower era bound accepted");
}

} // namespace

int main()
{
    TestAbiLayout();
    TestValidation();
    TestAcceptedSemanticRegistry();
    TestRegistrationAndReadback();
    TestCompositionAndConflict();
    TestDuplicateEffectiveAbilityAndBridge();
    TestHorsebackRidingReplacement();
    TestQueryErrors();
    return 0;
}
