#include "unit_combat_rules_registry.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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

ReRevvedUnitCombatRule MakeRule(const char* provider,
                                const char* rule_id,
                                ReRevvedUnitCombatProperty property,
                                int32_t percentage_delta)
{
    ReRevvedUnitCombatRule rule{};
    rule.struct_size      = sizeof(rule);
    rule.civilization     = REREVVED_CIVILIZATION_AZTEC;
    rule.base_unit_type   = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity         = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.terrain          = REREVVED_TERRAIN_FOREST;
    rule.property         = property;
    rule.percentage_delta = percentage_delta;
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    return rule;
}

ReRevvedUnitCombatEvaluation Evaluate(
    ReRevvedUnitCombatProperty property = REREVVED_UNIT_COMBAT_ATTACK)
{
    const ReRevvedUnitCombatQuery query = {
        sizeof(ReRevvedUnitCombatQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        REREVVED_TERRAIN_FOREST,
        property,
        {},
    };
    ReRevvedUnitCombatEvaluation evaluation{};
    Require(ReRevvedEvaluateUnitCombat(&query,
                                       &evaluation,
                                       sizeof(evaluation)) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "combat evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(ReRevvedUnitCombatRule) == 168);
    static_assert(offsetof(ReRevvedUnitCombatRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUnitCombatRule, terrain) == 144);
    static_assert(offsetof(ReRevvedUnitCombatRule, percentage_delta) == 152);
    static_assert(sizeof(ReRevvedUnitCombatRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitCombatRuleInfo, status_flags) == 156);
    static_assert(sizeof(ReRevvedUnitCombatQuery) == 40);
    static_assert(sizeof(ReRevvedUnitCombatEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitCombatEvaluation, final_percent) == 8);
    Require(ReRevvedUnitCombatRulesAbiVersion() ==
                REREVVED_UNIT_COMBAT_RULES_ABI_VERSION,
            "combat ABI version mismatch");

    rerevved::unit_combat_rules::ResetForTests();
    Require(ReRevvedRegisterUnitCombatRule(nullptr) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "null combat rule accepted");

    auto invalid    = MakeRule("test.provider",
                               "invalid",
                               REREVVED_UNIT_COMBAT_ATTACK,
                               1);
    invalid.terrain = REREVVED_TERRAIN_PLAINS;
    Require(ReRevvedRegisterUnitCombatRule(&invalid) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "non-Forest combat rule accepted");

    invalid             = MakeRule("test.provider",
                                   "invalid",
                                   REREVVED_UNIT_COMBAT_ATTACK,
                                   1);
    invalid.reserved[0] = 1;
    Require(ReRevvedRegisterUnitCombatRule(&invalid) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero combat reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto later = MakeRule("z.provider",
                                "late",
                                REREVVED_UNIT_COMBAT_ATTACK,
                                -25);
    auto earlier     = MakeRule("a.provider",
                                "early",
                                REREVVED_UNIT_COMBAT_ATTACK,
                                50);
    Require(ReRevvedRegisterUnitCombatRule(&later) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                ReRevvedRegisterUnitCombatRule(&earlier) ==
                    REREVVED_UNIT_COMBAT_RULES_OK,
            "combat rules did not register");
    Require(ReRevvedRegisterUnitCombatRule(&earlier) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "idempotent combat registration failed");

    earlier.percentage_delta = 40;
    Require(ReRevvedRegisterUnitCombatRule(&earlier) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting combat rule id accepted");

    uint32_t count = 0;
    Require(ReRevvedGetUnitCombatRuleCount(&count) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                count == 2,
            "combat rule count mismatch");
    ReRevvedUnitCombatRuleInfo info{};
    Require(ReRevvedGetUnitCombatRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.terrain == REREVVED_TERRAIN_FOREST &&
                info.percentage_delta == 50,
            "combat readback ordering mismatch");

    const auto evaluation = Evaluate();
    Require(evaluation.native_percent == 100 &&
                evaluation.final_percent == 125 &&
                evaluation.additive_count == 2 && evaluation.status_flags == 0,
            "combat additive composition mismatch");

    const auto defense = Evaluate(REREVVED_UNIT_COMBAT_DEFENSE);
    Require(defense.final_percent == defense.native_percent &&
                defense.additive_count == 0,
            "combat property target leaked to defense");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto overflow = MakeRule("a.provider",
                                   "overflow",
                                   REREVVED_UNIT_COMBAT_ATTACK,
                                   std::numeric_limits<int32_t>::max());
    Require(ReRevvedRegisterUnitCombatRule(&overflow) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "combat overflow rule did not register");
    const auto evaluation = Evaluate();
    Require(evaluation.final_percent == evaluation.native_percent &&
                (evaluation.status_flags &
                 REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0,
            "combat overflow did not preserve native percent");

    rerevved::unit_combat_rules::ResetForTests();
    const auto nonpositive = MakeRule("a.provider",
                                      "nonpositive",
                                      REREVVED_UNIT_COMBAT_ATTACK,
                                      -100);
    Require(ReRevvedRegisterUnitCombatRule(&nonpositive) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "nonpositive combat rule did not register");
    const auto invalid = Evaluate();
    Require(invalid.final_percent == invalid.native_percent &&
                (invalid.status_flags &
                 REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0,
            "nonpositive combat percentage did not preserve native percent");

    const ReRevvedUnitCombatQuery query = {
        sizeof(ReRevvedUnitCombatQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        REREVVED_TERRAIN_FOREST,
        REREVVED_UNIT_COMBAT_ATTACK,
        {},
    };
    ReRevvedUnitCombatEvaluation output{};
    Require(ReRevvedEvaluateUnitCombat(&query, &output, 19) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL,
            "short combat evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    Require(ReRevvedEvaluateUnitCombat(&query, &output, 20) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                output.struct_size == sizeof(output),
            "combat minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        Require(bytes[index] == 0x5a,
                "combat minimum prefix overwrote caller tail");
    }
}

} // namespace

int main()
{
    TestLayoutAndValidation();
    TestRegistrationReadbackAndEvaluation();
    TestOverflowAndSizedOutput();
    return 0;
}
