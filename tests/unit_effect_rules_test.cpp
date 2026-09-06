#include "unit_effect_rules_registry.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

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

ReRevvedUnitEffectRule MakeRule(const char*            provider,
                                const char*            rule_id,
                                ReRevvedCivilizationId civilization,
                                ReRevvedUnitTypeId     base_unit_type,
                                ReRevvedUnitIdentityId identity)
{
    ReRevvedUnitEffectRule rule{};
    rule.struct_size = sizeof(rule);
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    rule.civilization   = civilization;
    rule.base_unit_type = base_unit_type;
    rule.identity       = identity;
    rule.effect         = REREVVED_UNIT_EFFECT_CREATION_VETERAN;
    return rule;
}

ReRevvedUnitEffectEvaluation Evaluate(ReRevvedCivilizationId civilization,
                                      ReRevvedUnitTypeId     base_unit_type,
                                      ReRevvedUnitIdentityId identity,
                                      int32_t                native_level)
{
    const ReRevvedUnitEffectQuery query = {
        sizeof(ReRevvedUnitEffectQuery),
        civilization,
        base_unit_type,
        identity,
        REREVVED_UNIT_EFFECT_CREATION_VETERAN,
        native_level,
        {},
    };
    ReRevvedUnitEffectEvaluation evaluation{};
    Require(ReRevvedEvaluateUnitEffect(
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
    static_assert(offsetof(ReRevvedUnitEffectRuleInfo, status_flags) == 148);
    static_assert(sizeof(ReRevvedUnitEffectQuery) == 44);
    static_assert(offsetof(ReRevvedUnitEffectQuery, identity) == 12);
    static_assert(offsetof(ReRevvedUnitEffectQuery, native_level) == 20);
    static_assert(sizeof(ReRevvedUnitEffectEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitEffectEvaluation, final_level) == 8);
    Require(ReRevvedUnitEffectRulesAbiVersion() ==
                REREVVED_UNIT_EFFECT_RULES_ABI_VERSION,
            "unit effect ABI version mismatch");

    rerevved::unit_effect_rules::ResetForTests();
    auto invalid   = MakeRule("test.provider",
                              "invalid",
                              REREVVED_CIVILIZATION_AZTEC,
                              REREVVED_UNIT_TYPE_WARRIOR,
                              REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    invalid.effect = 2;
    Require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "unsupported unit effect accepted");
    invalid = MakeRule("test.provider",
                       "invalid",
                       REREVVED_CIVILIZATION_UNKNOWN,
                       REREVVED_UNIT_TYPE_WARRIOR,
                       REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    Require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "unknown unit effect civilization accepted");
    invalid = MakeRule("test.provider",
                       "invalid",
                       REREVVED_CIVILIZATION_AZTEC,
                       REREVVED_UNIT_TYPE_WARRIOR,
                       REREVVED_UNIT_IDENTITY_BASE);
    Require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "base identity unit effect accepted");
    invalid.identity = REREVVED_UNIT_IDENTITY_IMPI_WARRIOR;
    Require(ReRevvedRegisterUnitEffectRule(&invalid) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT,
            "mismatched identity unit effect accepted");
}

void TestRegistrationAndGrant()
{
    rerevved::unit_effect_rules::ResetForTests();
    auto later   = MakeRule("z.provider",
                            "late",
                            REREVVED_CIVILIZATION_AZTEC,
                            REREVVED_UNIT_TYPE_WARRIOR,
                            REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    auto earlier = MakeRule("a.provider",
                            "early",
                            REREVVED_CIVILIZATION_AZTEC,
                            REREVVED_UNIT_TYPE_WARRIOR,
                            REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    Require(ReRevvedRegisterUnitEffectRule(&later) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                ReRevvedRegisterUnitEffectRule(&earlier) ==
                    REREVVED_UNIT_EFFECT_RULES_OK,
            "unit effect rules did not register");

    uint32_t count = 0;
    Require(ReRevvedGetUnitEffectRuleCount(&count) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                count == 2,
            "unit effect rule count mismatch");
    ReRevvedUnitEffectRuleInfo info{};
    Require(ReRevvedGetUnitEffectRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.civilization == REREVVED_CIVILIZATION_AZTEC &&
                info.base_unit_type == REREVVED_UNIT_TYPE_WARRIOR &&
                info.identity == REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR &&
                info.effect == REREVVED_UNIT_EFFECT_CREATION_VETERAN,
            "unit effect readback ordering mismatch");

    const auto granted = Evaluate(REREVVED_CIVILIZATION_AZTEC,
                                  REREVVED_UNIT_TYPE_WARRIOR,
                                  REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                  1);
    Require(granted.final_level == 2 && granted.grant_count == 2 &&
                (granted.status_flags &
                 REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0,
            "Veteran grant did not raise lower rank to level two");
    const auto preserved = Evaluate(REREVVED_CIVILIZATION_AZTEC,
                                    REREVVED_UNIT_TYPE_WARRIOR,
                                    REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
                                    3);
    Require(preserved.final_level == 3,
            "Veteran grant lowered native rank");
    const auto wrong_target = Evaluate(REREVVED_CIVILIZATION_ZULU,
                                       REREVVED_UNIT_TYPE_WARRIOR,
                                       REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
                                       1);
    Require(wrong_target.final_level == 1 && wrong_target.grant_count == 0,
            "Veteran grant crossed civilization target");
}

void TestSizedOutput()
{
    rerevved::unit_effect_rules::ResetForTests();
    const auto rule = MakeRule("a.provider",
                               "grant",
                               REREVVED_CIVILIZATION_AZTEC,
                               REREVVED_UNIT_TYPE_WARRIOR,
                               REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR);
    Require(ReRevvedRegisterUnitEffectRule(&rule) ==
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
    Require(ReRevvedEvaluateUnitEffect(&query, &output, 19) ==
                REREVVED_UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL,
            "short unit effect evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    Require(ReRevvedEvaluateUnitEffect(&query, &output, 20) ==
                    REREVVED_UNIT_EFFECT_RULES_OK &&
                output.struct_size ==
                    sizeof(output),
            "unit effect minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        Require(bytes[index] == 0x5a,
                "unit effect minimum prefix overwrote caller tail");
    }
}

} // namespace

int main()
{
    TestLayoutAndValidation();
    TestRegistrationAndGrant();
    TestSizedOutput();
    return 0;
}
