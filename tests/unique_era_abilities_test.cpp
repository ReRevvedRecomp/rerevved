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

void ReRevvedApplyUniqueEraAbilityCell(PPCRegister& cell_offset,
                                       PPCRegister& unlock_era,
                                       PPCRegister& native_ability);

namespace
{

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedUniqueEraAbilityReplacement MakeRule(
    const char*                provider,
    const char*                rule_id,
    ReRevvedCivilizationId     civilization,
    ReRevvedUniqueEraUnlockEra unlock_era,
    ReRevvedUniqueEraAbilityId replacement)
{
    ReRevvedUniqueEraAbilityReplacement rule{};
    rule.struct_size = sizeof(rule);
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    rule.civilization        = civilization;
    rule.unlock_era          = unlock_era;
    rule.replacement_ability = replacement;
    return rule;
}

ReRevvedUniqueEraAbilityCellEvaluation Evaluate(
    ReRevvedCivilizationId     civilization,
    ReRevvedUniqueEraUnlockEra unlock_era,
    ReRevvedUniqueEraAbilityId native_ability)
{
    const ReRevvedUniqueEraAbilityCellQuery query = {
        sizeof(ReRevvedUniqueEraAbilityCellQuery),
        civilization,
        unlock_era,
        native_ability,
        {},
    };
    ReRevvedUniqueEraAbilityCellEvaluation result{};
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &result, sizeof(result)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "cell evaluation failed");
    return result;
}

void TestAbiLayout()
{
    static_assert(sizeof(ReRevvedUniqueEraAbilityReplacement) == 176);
    static_assert(offsetof(ReRevvedUniqueEraAbilityReplacement, provider_id) ==
                  4);
    static_assert(offsetof(ReRevvedUniqueEraAbilityReplacement, rule_id) == 68);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityReplacement, civilization) == 132);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityReplacement, replacement_ability) ==
        140);
    static_assert(sizeof(ReRevvedUniqueEraAbilityRuleInfo) == 180);
    static_assert(
        offsetof(ReRevvedUniqueEraAbilityRuleInfo, status_flags) == 144);
    static_assert(sizeof(ReRevvedUniqueEraAbilityCellQuery) == 40);
    static_assert(sizeof(ReRevvedUniqueEraAbilityCellEvaluation) == 40);
    static_assert(offsetof(ReRevvedUniqueEraAbilityCellEvaluation,
                           effective_ability) == 8);
    Require(ReRevvedUniqueEraAbilitiesAbiVersion() ==
                REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION,
            "ABI version mismatch");
}

void TestValidation()
{
    rerevved::unique_era_abilities::ResetForTests();
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(nullptr) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null rule accepted");

    auto rule = MakeRule("aeshur.roman-rush-test",
                         "roman-medieval-rush",
                         REREVVED_CIVILIZATION_ROMAN,
                         REREVVED_UNIQUE_ERA_MEDIEVAL,
                         REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    rule.struct_size--;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "short rule accepted");
    rule.struct_size++;

    rule.provider_id[0] = 'A';
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "malformed provider accepted");
    constexpr char provider[] = "aeshur.roman-rush-test";
    std::memcpy(rule.provider_id, provider, sizeof(provider));
    rule.civilization = REREVVED_CIVILIZATION_UNKNOWN;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "unknown civilization accepted");
    rule.civilization = REREVVED_CIVILIZATION_ROMAN;
    rule.unlock_era   = 4;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "unknown era accepted");
    rule.unlock_era          = REREVVED_UNIQUE_ERA_MEDIEVAL;
    rule.replacement_ability = 11;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "non-retail UEA accepted");
    rule.replacement_ability =
        REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST;
    rule.reserved[7] = 1;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&rule) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field accepted");

    uint32_t count = 0;
    Require(ReRevvedGetUniqueEraAbilityRuleCount(nullptr) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null count accepted");
    Require(ReRevvedGetUniqueEraAbilityRuleCount(&count) ==
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
        query.struct_size    = sizeof(query);
        query.civilization   = REREVVED_CIVILIZATION_ROMAN;
        query.unlock_era     = REREVVED_UNIQUE_ERA_ANCIENT;
        query.native_ability = ability;
        ReRevvedUniqueEraAbilityCellEvaluation result{};
        const bool                             expected =
            std::find(abilities.begin(), abilities.end(), ability) !=
            abilities.end();
        Require((ReRevvedEvaluateUniqueEraAbilityCell(
                     &query, &result, sizeof(result)) ==
                 REREVVED_UNIQUE_ERA_ABILITIES_OK) == expected,
                "semantic registry accepted the wrong numeric UEA set");
    }
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
                const auto result = Evaluate(civilization, era, ability);
                Require(result.native_ability == ability &&
                            result.effective_ability == ability &&
                            result.replacement_count == 0 &&
                            result.status_flags == 0,
                        "accepted no-rule cell changed");
            }
        }
    }
}

void TestRegistrationAndReadback()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto roman   = MakeRule("z.provider",
                            "roman-medieval-rush",
                            REREVVED_CIVILIZATION_ROMAN,
                            REREVVED_UNIQUE_ERA_MEDIEVAL,
                            REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english = MakeRule("a.provider",
                            "english-ancient-rush",
                            REREVVED_CIVILIZATION_ENGLISH,
                            REREVVED_UNIQUE_ERA_ANCIENT,
                            REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&english) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "valid rules rejected");

    std::memset(roman.provider_id + std::strlen(roman.provider_id) + 1,
                'x',
                sizeof(roman.provider_id) - std::strlen(roman.provider_id) - 1);
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "normalized duplicate was not idempotent");
    roman.replacement_ability = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&roman) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate key accepted");

    uint32_t count = 0;
    Require(ReRevvedGetUniqueEraAbilityRuleCount(&count) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                count == 2,
            "wrong rule count");
    ReRevvedUniqueEraAbilityRuleInfo info{};
    Require(ReRevvedGetUniqueEraAbilityRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                std::strcmp(info.provider_id, "a.provider") == 0,
            "readback is not canonical");
    Require(ReRevvedGetUniqueEraAbilityRule(0, &info, 147) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL,
            "short readback buffer accepted");
    Require(ReRevvedGetUniqueEraAbilityRule(0, &info, 148) ==
                    REREVVED_UNIQUE_ERA_ABILITIES_OK &&
                info.struct_size == sizeof(info),
            "minimum readback prefix rejected");
}

void TestCompositionAndConflict()
{
    auto roman    = MakeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto english  = MakeRule("example.english",
                             "english-modern-rush",
                             REREVVED_CIVILIZATION_ENGLISH,
                             REREVVED_UNIQUE_ERA_MODERN,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto conflict = MakeRule("example.conflict",
                             "roman-medieval-pottery",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_POTTERY);

    rerevved::unique_era_abilities::ResetForTests();
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&english) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&roman) == 0,
            "distinct cells failed registration");
    auto roman_result   = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                                   REREVVED_UNIQUE_ERA_MEDIEVAL,
                                   REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    auto english_result = Evaluate(
        REREVVED_CIVILIZATION_ENGLISH,
        REREVVED_UNIQUE_ERA_MODERN,
        REREVVED_UNIQUE_ERA_ABILITY_DOUBLE_NAVAL_SUPPORT);
    Require(roman_result.effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST &&
                english_result.effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "distinct cells did not compose");

    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&conflict) == 0,
            "conflicting replacement registration failed");
    roman_result = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                            REREVVED_UNIQUE_ERA_MEDIEVAL,
                            REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    Require(roman_result.effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST &&
                roman_result.replacement_count == 2 &&
                roman_result.status_flags ==
                    REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT,
            "same-cell conflict did not restore native ability");

    for (uint32_t index = 0; index < 3; ++index)
    {
        ReRevvedUniqueEraAbilityRuleInfo info{};
        Require(ReRevvedGetUniqueEraAbilityRule(index, &info, sizeof(info)) == 0,
                "conflict readback failed");
        const bool roman_cell =
            info.civilization == REREVVED_CIVILIZATION_ROMAN &&
            info.unlock_era == REREVVED_UNIQUE_ERA_MEDIEVAL;
        Require(!roman_cell ||
                    info.status_flags ==
                        REREVVED_UNIQUE_ERA_ABILITY_RULE_REPLACEMENT_CONFLICT,
                "conflict was not disclosed on readback");
    }

    rerevved::unique_era_abilities::ResetForTests();
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&conflict) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&roman) == 0,
            "reverse conflict order failed registration");
    roman_result = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                            REREVVED_UNIQUE_ERA_MEDIEVAL,
                            REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST);
    Require(roman_result.effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST &&
                roman_result.replacement_count == 2,
            "conflict depended on registration order");
}

void TestDuplicateEffectiveAbilityAndBridge()
{
    rerevved::unique_era_abilities::ResetForTests();
    auto ancient  = MakeRule("example.roman",
                             "roman-ancient-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_ANCIENT,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    auto medieval = MakeRule("aeshur.roman-rush-test",
                             "roman-medieval-rush",
                             REREVVED_CIVILIZATION_ROMAN,
                             REREVVED_UNIQUE_ERA_MEDIEVAL,
                             REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST);
    Require(ReRevvedRegisterUniqueEraAbilityReplacement(&ancient) == 0 &&
                ReRevvedRegisterUniqueEraAbilityReplacement(&medieval) == 0,
            "duplicate-effective cells failed registration");
    Require(Evaluate(REREVVED_CIVILIZATION_ROMAN,
                     REREVVED_UNIQUE_ERA_ANCIENT,
                     REREVVED_UNIQUE_ERA_ABILITY_ROADS_HALF_COST)
                        .effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST &&
                Evaluate(REREVVED_CIVILIZATION_ROMAN,
                         REREVVED_UNIQUE_ERA_MEDIEVAL,
                         REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST)
                        .effective_ability ==
                    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "duplicate effective UEA was suppressed");

    PPCRegister offset{};
    PPCRegister era{};
    PPCRegister ability{};
    offset.u64  = 4;
    era.s64     = REREVVED_UNIQUE_ERA_MEDIEVAL;
    ability.s64 = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    Require(ability.s32 ==
                REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST,
            "Roman Medieval bridge did not apply 24 -> 35");

    offset.u64  = 6;
    ability.s64 = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    Require(ability.s32 == REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST,
            "unaligned bridge offset changed native value");

    offset.u64 = 4;
    era.s64    = 4;
    ReRevvedApplyUniqueEraAbilityCell(offset, era, ability);
    Require(ability.s32 == REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST,
            "invalid bridge era changed native value");
}

void TestQueryErrors()
{
    rerevved::unique_era_abilities::ResetForTests();
    ReRevvedUniqueEraAbilityCellQuery query{};
    query.struct_size    = sizeof(query);
    query.civilization   = REREVVED_CIVILIZATION_ROMAN;
    query.unlock_era     = REREVVED_UNIQUE_ERA_MEDIEVAL;
    query.native_ability = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    ReRevvedUniqueEraAbilityCellEvaluation out{};
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, nullptr, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "null output accepted");
    Require(ReRevvedEvaluateUniqueEraAbilityCell(&query, &out, 19) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL,
            "short output buffer accepted");
    Require(ReRevvedEvaluateUniqueEraAbilityCell(&query, &out, 20) ==
                REREVVED_UNIQUE_ERA_ABILITIES_OK,
            "minimum output prefix rejected");
    query.native_ability = 11;
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "non-retail native UEA accepted");
    query.native_ability = REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST;
    query.reserved[0]    = 1;
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "nonzero query reserve accepted");
    query.reserved[0]  = 0;
    query.civilization = REREVVED_CIVILIZATION_COUNT;
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
                &query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT,
            "upper civilization bound accepted");
    query.civilization = REREVVED_CIVILIZATION_ROMAN;
    query.unlock_era   = -1;
    Require(ReRevvedEvaluateUniqueEraAbilityCell(
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
    TestQueryErrors();
    return 0;
}
