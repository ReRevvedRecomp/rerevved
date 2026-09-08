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

UnitCombatRule makeRule(const char*        provider,
                        const char*        ruleId,
                        UnitCombatProperty property,
                        int32_t            percentageDelta)
{
    UnitCombatRule rule{};
    rule.structSize      = sizeof(rule);
    rule.civilization    = CIVILIZATION_AZTEC;
    rule.baseUnitType    = UNIT_TYPE_WARRIOR;
    rule.identity        = UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.terrain         = TERRAIN_FOREST;
    rule.property        = property;
    rule.percentageDelta = percentageDelta;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    return rule;
}

UnitCombatEvaluation evaluate(
    UnitCombatProperty property = UNIT_COMBAT_ATTACK)
{
    const UnitCombatQuery query = {
        sizeof(UnitCombatQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        TERRAIN_FOREST,
        property,
        {},
    };
    UnitCombatEvaluation evaluation{};
    require(EvaluateUnitCombat(&query,
                               &evaluation,
                               sizeof(evaluation)) ==
                UNIT_COMBAT_RULES_OK,
            "combat evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(UnitCombatRule) == 168);
    static_assert(offsetof(UnitCombatRule, civilization) == 132);
    static_assert(offsetof(UnitCombatRule, terrain) == 144);
    static_assert(offsetof(UnitCombatRule, percentageDelta) == 152);
    static_assert(sizeof(UnitCombatRuleInfo) == 192);
    static_assert(offsetof(UnitCombatRuleInfo, statusFlags) == 156);
    static_assert(sizeof(UnitCombatQuery) == 40);
    static_assert(sizeof(UnitCombatEvaluation) == 40);
    static_assert(offsetof(UnitCombatEvaluation, finalPercent) == 8);
    require(UnitCombatRulesAbiVersion() ==
                UNIT_COMBAT_RULES_ABI_VERSION,
            "combat ABI version mismatch");

    rerevved::unit_combat_rules::ResetForTests();
    require(RegisterUnitCombatRule(nullptr) ==
                UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "null combat rule accepted");

    auto invalid    = makeRule("test.provider",
                               "invalid",
                               UNIT_COMBAT_ATTACK,
                               1);
    invalid.terrain = TERRAIN_PLAINS;
    require(RegisterUnitCombatRule(&invalid) ==
                UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "non-Forest combat rule accepted");

    invalid             = makeRule("test.provider",
                                   "invalid",
                                   UNIT_COMBAT_ATTACK,
                                   1);
    invalid.reserved[0] = 1;
    require(RegisterUnitCombatRule(&invalid) ==
                UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero combat reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto later   = makeRule("z.provider",
                                  "late",
                                  UNIT_COMBAT_ATTACK,
                                  -25);
    auto       earlier = makeRule("a.provider",
                                  "early",
                                  UNIT_COMBAT_ATTACK,
                                  50);
    require(RegisterUnitCombatRule(&later) ==
                    UNIT_COMBAT_RULES_OK &&
                RegisterUnitCombatRule(&earlier) ==
                    UNIT_COMBAT_RULES_OK,
            "combat rules did not register");
    require(RegisterUnitCombatRule(&earlier) ==
                UNIT_COMBAT_RULES_OK,
            "idempotent combat registration failed");

    earlier.percentageDelta = 40;
    require(RegisterUnitCombatRule(&earlier) ==
                UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting combat rule id accepted");

    uint32_t count = 0;
    require(GetUnitCombatRuleCount(&count) ==
                    UNIT_COMBAT_RULES_OK &&
                count == 2,
            "combat rule count mismatch");
    UnitCombatRuleInfo info{};
    require(GetUnitCombatRule(0, &info, sizeof(info)) ==
                    UNIT_COMBAT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.terrain == TERRAIN_FOREST &&
                info.percentageDelta == 50,
            "combat readback ordering mismatch");

    const auto evaluation = evaluate();
    require(evaluation.nativePercent == 100 &&
                evaluation.finalPercent == 125 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "combat additive composition mismatch");

    const auto defense = evaluate(UNIT_COMBAT_DEFENSE);
    require(defense.finalPercent == defense.nativePercent &&
                defense.additiveCount == 0,
            "combat property target leaked to defense");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto overflow = makeRule("a.provider",
                                   "overflow",
                                   UNIT_COMBAT_ATTACK,
                                   std::numeric_limits<int32_t>::max());
    require(RegisterUnitCombatRule(&overflow) ==
                UNIT_COMBAT_RULES_OK,
            "combat overflow rule did not register");
    const auto evaluation = evaluate();
    require(evaluation.finalPercent == evaluation.nativePercent &&
                (evaluation.statusFlags &
                 UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0,
            "combat overflow did not preserve native percent");

    rerevved::unit_combat_rules::ResetForTests();
    const auto nonpositive = makeRule("a.provider",
                                      "nonpositive",
                                      UNIT_COMBAT_ATTACK,
                                      -100);
    require(RegisterUnitCombatRule(&nonpositive) ==
                UNIT_COMBAT_RULES_OK,
            "nonpositive combat rule did not register");
    const auto invalid = evaluate();
    require(invalid.finalPercent == invalid.nativePercent &&
                (invalid.statusFlags &
                 UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0,
            "nonpositive combat percentage did not preserve native percent");

    const UnitCombatQuery query = {
        sizeof(UnitCombatQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        TERRAIN_FOREST,
        UNIT_COMBAT_ATTACK,
        {},
    };
    UnitCombatEvaluation output{};
    require(EvaluateUnitCombat(&query, &output, 19) ==
                UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL,
            "short combat evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(EvaluateUnitCombat(&query, &output, 20) ==
                    UNIT_COMBAT_RULES_OK &&
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
