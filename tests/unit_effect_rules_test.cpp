#include "unit_effect_rules_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

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

ReRevvedUnitEffectRule makeRule(const char*            provider,
                                const char*            ruleId,
                                ReRevvedCivilizationId civilization,
                                ReRevvedUnitTypeId     baseUnitType,
                                ReRevvedUnitIdentityId identity,
                                ReRevvedUnitEffectId   effect =
                                    REREVVED_UNIT_EFFECT_CREATION_VETERAN)
{
    ReRevvedUnitEffectRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization = civilization;
    rule.baseUnitType = baseUnitType;
    rule.identity     = identity;
    rule.effect       = effect;
    return rule;
}

ReRevvedUnitEffectEvaluation evaluate(ReRevvedCivilizationId civilization,
                                      ReRevvedUnitTypeId     baseUnitType,
                                      ReRevvedUnitIdentityId identity,
                                      int32_t                nativeLevel,
                                      ReRevvedUnitEffectId   effect =
                                          REREVVED_UNIT_EFFECT_CREATION_VETERAN)
{
    const ReRevvedUnitEffectQuery query = {
        sizeof(ReRevvedUnitEffectQuery),
        civilization,
        baseUnitType,
        identity,
        effect,
        nativeLevel,
        {},
    };
    ReRevvedUnitEffectEvaluation evaluation{};
    require(ReRevvedEvaluateUnitEffect(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_UNIT_EFFECT_RULES_OK,
            "unit effect evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(ReRevvedUnitEffectRule) == 168);
    static_assert(offsetof(ReRevvedUnitEffectRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUnitEffectRule, identity) == 140);
    static_assert(offsetof(ReRevvedUnitEffectRule, effect) == 144);
    static_assert(sizeof(ReRevvedUnitEffectRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitEffectRuleInfo, identity) == 140);
    static_assert(offsetof(ReRevvedUnitEffectRuleInfo, statusFlags) == 148);
    static_assert(sizeof(ReRevvedUnitEffectQuery) == 44);
    static_assert(offsetof(ReRevvedUnitEffectQuery, identity) == 12);
    static_assert(offsetof(ReRevvedUnitEffectQuery, nativeLevel) == 20);
    static_assert(sizeof(ReRevvedUnitEffectEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitEffectEvaluation, finalLevel) == 8);
    require(ReRevvedUnitEffectRulesAbiVersion() ==
                REREVVED_UNIT_EFFECT_RULES_ABI_VERSION,
            "unit effect ABI version mismatch");

    rerevved::unit_effect_rules::ResetForTests();
    auto invalid   = makeRule("test.provider",
                              "invalid",
                              REREVVED_CIVILIZATION_AZTEC,
                              REREVVED_UNIT_TYPE_WARRIOR,
                              REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    invalid.effect = 11;
    require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "unsupported unit effect accepted");
    invalid = makeRule("test.provider",
                       "invalid",
                       REREVVED_CIVILIZATION_UNKNOWN,
                       REREVVED_UNIT_TYPE_WARRIOR,
                       REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "unknown unit effect civilization accepted");
    invalid = makeRule("test.provider",
                       "invalid",
                       REREVVED_CIVILIZATION_AZTEC,
                       REREVVED_UNIT_TYPE_WARRIOR,
                       REREVVED_UNIT_IDENTITY_BASE);
    require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "base identity unit effect accepted");
    invalid.identity = REREVVED_UNIT_IDENTITY_IMPI_WARRIOR;
    require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "mismatched identity unit effect accepted");
}

void TestNamedSpecialEffects()
{
    struct ExpectedEffect
    {
        ReRevvedUnitEffectId effect;
        uint32_t             mask;
    };

    constexpr std::array<ExpectedEffect, 9> expected = { {
        { REREVVED_UNIT_EFFECT_CREATION_GUERILLA, 1u << 2 },
        { REREVVED_UNIT_EFFECT_CREATION_BLITZ, 1u << 0 },
        { REREVVED_UNIT_EFFECT_CREATION_INFILTRATION, 1u << 1 },
        { REREVVED_UNIT_EFFECT_CREATION_LOYALTY, 1u << 3 },
        { REREVVED_UNIT_EFFECT_CREATION_ENGINEER, 1u << 4 },
        { REREVVED_UNIT_EFFECT_CREATION_LEADERSHIP, 1u << 5 },
        { REREVVED_UNIT_EFFECT_CREATION_MARCH, 1u << 6 },
        { REREVVED_UNIT_EFFECT_CREATION_MEDIC, 1u << 7 },
        { REREVVED_UNIT_EFFECT_CREATION_SCOUT, 1u << 8 },
    } };

    rerevved::unit_effect_rules::ResetForTests();
    constexpr std::array<const char*, 9> ruleIds = { {
        "guerilla",
        "blitz",
        "infiltration",
        "loyalty",
        "engineer",
        "leadership",
        "march",
        "medic",
        "scout",
    } };

    uint32_t veteranMask = 0;
    require(!rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
                REREVVED_UNIT_EFFECT_CREATION_VETERAN,
                veteranMask),
            "Veteran was treated as a native special upgrade");

    for (size_t index = 0; index < expected.size(); ++index)
    {
        uint32_t mask = 0;
        require(rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
                    expected[index].effect,
                    mask) &&
                    mask == expected[index].mask,
                "named special effect mapped to the wrong native mask");

        const auto rule = makeRule("test.special",
                                   ruleIds[index],
                                   REREVVED_CIVILIZATION_AZTEC,
                                   REREVVED_UNIT_TYPE_WARRIOR,
                                   REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                   expected[index].effect);
        require(ReRevvedRegisterUnitEffectRule(&rule) ==
                    REREVVED_UNIT_EFFECT_RULES_OK,
                "named special effect registration failed");

        const auto evaluation = evaluate(REREVVED_CIVILIZATION_AZTEC,
                                         REREVVED_UNIT_TYPE_WARRIOR,
                                         REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                         1,
                                         expected[index].effect);
        require(evaluation.finalLevel == 1 &&
                    evaluation.grantCount == 1 &&
                    (evaluation.statusFlags &
                     REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0,
                "special effect changed the native rank");
    }
}

void TestRegistrationAndGrant()
{
    rerevved::unit_effect_rules::ResetForTests();
    auto later   = makeRule("z.provider",
                            "late",
                            REREVVED_CIVILIZATION_AZTEC,
                            REREVVED_UNIT_TYPE_WARRIOR,
                            REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    auto earlier = makeRule("a.provider",
                            "early",
                            REREVVED_CIVILIZATION_AZTEC,
                            REREVVED_UNIT_TYPE_WARRIOR,
                            REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    require(ReRevvedRegisterUnitEffectRule(&later) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                ReRevvedRegisterUnitEffectRule(&earlier) ==
                    REREVVED_UNIT_EFFECT_RULES_OK,
            "unit effect rules did not register");

    uint32_t count = 0;
    require(ReRevvedGetUnitEffectRuleCount(&count) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                count == 2,
            "unit effect rule count mismatch");
    ReRevvedUnitEffectRuleInfo info{};
    require(ReRevvedGetUnitEffectRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.civilization == REREVVED_CIVILIZATION_AZTEC &&
                info.baseUnitType == REREVVED_UNIT_TYPE_WARRIOR &&
                info.identity == REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR &&
                info.effect == REREVVED_UNIT_EFFECT_CREATION_VETERAN,
            "unit effect readback ordering mismatch");

    const auto granted = evaluate(REREVVED_CIVILIZATION_AZTEC,
                                  REREVVED_UNIT_TYPE_WARRIOR,
                                  REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                  1);
    require(granted.finalLevel == 2 && granted.grantCount == 2 &&
                (granted.statusFlags &
                 REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0,
            "Veteran grant did not raise lower rank to level two");
    const auto preserved = evaluate(REREVVED_CIVILIZATION_AZTEC,
                                    REREVVED_UNIT_TYPE_WARRIOR,
                                    REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                    3);
    require(preserved.finalLevel == 3,
            "Veteran grant lowered native rank");
    const auto wrongTarget = evaluate(REREVVED_CIVILIZATION_ZULU,
                                      REREVVED_UNIT_TYPE_WARRIOR,
                                      REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
                                      1);
    require(wrongTarget.finalLevel == 1 && wrongTarget.grantCount == 0,
            "Veteran grant crossed civilization target");
}

void TestSizedOutput()
{
    rerevved::unit_effect_rules::ResetForTests();
    const auto rule = makeRule("a.provider",
                               "grant",
                               REREVVED_CIVILIZATION_AZTEC,
                               REREVVED_UNIT_TYPE_WARRIOR,
                               REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    require(ReRevvedRegisterUnitEffectRule(&rule) ==
                REREVVED_UNIT_EFFECT_RULES_OK,
            "unit effect rule did not register");
    const ReRevvedUnitEffectQuery query = {
        sizeof(ReRevvedUnitEffectQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        REREVVED_UNIT_EFFECT_CREATION_VETERAN,
        1,
        {},
    };
    ReRevvedUnitEffectEvaluation output{};
    require(ReRevvedEvaluateUnitEffect(&query, &output, 19) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL,
            "short unit effect evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(ReRevvedEvaluateUnitEffect(&query, &output, 20) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                output.structSize ==
                    sizeof(output),
            "unit effect minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        require(bytes[index] == 0x5a,
                "unit effect minimum prefix overwrote caller tail");
    }
}

} // namespace

int main()
{
    TestLayoutAndValidation();
    TestNamedSpecialEffects();
    TestRegistrationAndGrant();
    TestSizedOutput();
    return 0;
}
