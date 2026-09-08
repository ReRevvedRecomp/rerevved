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

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedUnitCombatRule makeRule(const char*                provider,
                                const char*                ruleId,
                                ReRevvedUnitCombatProperty property,
                                int32_t                    percentageDelta)
{
    ReRevvedUnitCombatRule rule{};
    rule.structSize      = sizeof(rule);
    rule.civilization    = REREVVED_CIVILIZATION_AZTEC;
    rule.baseUnitType    = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity        = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.terrain         = REREVVED_TERRAIN_FOREST;
    rule.property        = property;
    rule.percentageDelta = percentageDelta;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    return rule;
}

ReRevvedUnitCombatEvaluation evaluate(
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
    require(ReRevvedEvaluateUnitCombat(&query,
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
    static_assert(offsetof(ReRevvedUnitCombatRule, percentageDelta) == 152);
    static_assert(sizeof(ReRevvedUnitCombatRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitCombatRuleInfo, statusFlags) == 156);
    static_assert(sizeof(ReRevvedUnitCombatQuery) == 40);
    static_assert(sizeof(ReRevvedUnitCombatEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitCombatEvaluation, finalPercent) == 8);
    require(ReRevvedUnitCombatRulesAbiVersion() ==
                REREVVED_UNIT_COMBAT_RULES_ABI_VERSION,
            "combat ABI version mismatch");

    rerevved::unit_combat_rules::ResetForTests();
    require(ReRevvedRegisterUnitCombatRule(nullptr) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "null combat rule accepted");

    auto invalid    = makeRule("test.provider",
                               "invalid",
                               REREVVED_UNIT_COMBAT_ATTACK,
                               1);
    invalid.terrain = REREVVED_TERRAIN_PLAINS;
    require(ReRevvedRegisterUnitCombatRule(&invalid) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "non-Forest combat rule accepted");

    invalid             = makeRule("test.provider",
                                   "invalid",
                                   REREVVED_UNIT_COMBAT_ATTACK,
                                   1);
    invalid.reserved[0] = 1;
    require(ReRevvedRegisterUnitCombatRule(&invalid) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero combat reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto later   = makeRule("z.provider",
                                  "late",
                                  REREVVED_UNIT_COMBAT_ATTACK,
                                  -25);
    auto       earlier = makeRule("a.provider",
                                  "early",
                                  REREVVED_UNIT_COMBAT_ATTACK,
                                  50);
    require(ReRevvedRegisterUnitCombatRule(&later) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                ReRevvedRegisterUnitCombatRule(&earlier) ==
                    REREVVED_UNIT_COMBAT_RULES_OK,
            "combat rules did not register");
    require(ReRevvedRegisterUnitCombatRule(&earlier) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "idempotent combat registration failed");

    earlier.percentageDelta = 40;
    require(ReRevvedRegisterUnitCombatRule(&earlier) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting combat rule id accepted");

    uint32_t count = 0;
    require(ReRevvedGetUnitCombatRuleCount(&count) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                count == 2,
            "combat rule count mismatch");
    ReRevvedUnitCombatRuleInfo info{};
    require(ReRevvedGetUnitCombatRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.terrain == REREVVED_TERRAIN_FOREST &&
                info.percentageDelta == 50,
            "combat readback ordering mismatch");

    const auto evaluation = evaluate();
    require(evaluation.nativePercent == 100 &&
                evaluation.finalPercent == 125 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "combat additive composition mismatch");

    const auto defense = evaluate(REREVVED_UNIT_COMBAT_DEFENSE);
    require(defense.finalPercent == defense.nativePercent &&
                defense.additiveCount == 0,
            "combat property target leaked to defense");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto overflow = makeRule("a.provider",
                                   "overflow",
                                   REREVVED_UNIT_COMBAT_ATTACK,
                                   std::numeric_limits<int32_t>::max());
    require(ReRevvedRegisterUnitCombatRule(&overflow) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "combat overflow rule did not register");
    const auto evaluation = evaluate();
    require(evaluation.finalPercent == evaluation.nativePercent &&
                (evaluation.statusFlags &
                 REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0,
            "combat overflow did not preserve native percent");

    rerevved::unit_combat_rules::ResetForTests();
    const auto nonpositive = makeRule("a.provider",
                                      "nonpositive",
                                      REREVVED_UNIT_COMBAT_ATTACK,
                                      -100);
    require(ReRevvedRegisterUnitCombatRule(&nonpositive) ==
                REREVVED_UNIT_COMBAT_RULES_OK,
            "nonpositive combat rule did not register");
    const auto invalid = evaluate();
    require(invalid.finalPercent == invalid.nativePercent &&
                (invalid.statusFlags &
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
    require(ReRevvedEvaluateUnitCombat(&query, &output, 19) ==
                REREVVED_UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL,
            "short combat evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(ReRevvedEvaluateUnitCombat(&query, &output, 20) ==
                    REREVVED_UNIT_COMBAT_RULES_OK &&
                output.structSize == sizeof(output),
            "combat minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        require(bytes[index] == 0x5a,
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
