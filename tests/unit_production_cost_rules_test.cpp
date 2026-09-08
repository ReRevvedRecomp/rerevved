#include "unit_production_cost_rules_registry.h"

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

UnitProductionCostRule makeRule(const char* provider,
                                const char* ruleId,
                                int32_t     percentageDelta)
{
    UnitProductionCostRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization    = CIVILIZATION_AZTEC;
    rule.baseUnitType    = UNIT_TYPE_WARRIOR;
    rule.identity        = UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.percentageDelta = percentageDelta;
    return rule;
}

UnitProductionCostEvaluation evaluate()
{
    const UnitProductionCostQuery query = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation evaluation{};
    require(EvaluateUnitProductionCost(
                &query, &evaluation, sizeof(evaluation)) ==
                UNIT_PRODUCTION_COST_RULES_OK,
            "production cost evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(UnitProductionCostRule) == 168);
    static_assert(offsetof(UnitProductionCostRule, civilization) == 132);
    static_assert(offsetof(UnitProductionCostRule,
                           percentageDelta) == 144);
    static_assert(sizeof(UnitProductionCostRuleInfo) == 192);
    static_assert(offsetof(UnitProductionCostRuleInfo, statusFlags) == 148);
    static_assert(sizeof(UnitProductionCostQuery) == 40);
    static_assert(sizeof(UnitProductionCostEvaluation) == 40);
    static_assert(offsetof(UnitProductionCostEvaluation, finalPercent) == 8);
    require(UnitProductionCostRulesAbiVersion() ==
                UNIT_PRODUCTION_COST_RULES_ABI_VERSION,
            "production cost ABI version mismatch");

    rerevved::unit_production_cost_rules::ResetForTests();
    require(RegisterUnitProductionCostRule(nullptr) ==
                UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "null production cost rule accepted");
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = UNIT_IDENTITY_BASE;
    require(RegisterUnitProductionCostRule(&invalid) ==
                UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "base identity production cost rule accepted");
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    require(RegisterUnitProductionCostRule(&invalid) ==
                UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "nonzero production cost reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -25);
    auto earlier = makeRule("a.provider", "early", 50);
    require(RegisterUnitProductionCostRule(&later) ==
                    UNIT_PRODUCTION_COST_RULES_OK &&
                RegisterUnitProductionCostRule(&earlier) ==
                    UNIT_PRODUCTION_COST_RULES_OK,
            "production cost rules did not register");
    require(RegisterUnitProductionCostRule(&earlier) ==
                UNIT_PRODUCTION_COST_RULES_OK,
            "idempotent production cost registration failed");
    earlier.percentageDelta = 40;
    require(RegisterUnitProductionCostRule(&earlier) ==
                UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting production cost rule id accepted");

    uint32_t count = 0;
    require(GetUnitProductionCostRuleCount(&count) ==
                    UNIT_PRODUCTION_COST_RULES_OK &&
                count == 2,
            "production cost rule count mismatch");
    UnitProductionCostRuleInfo info{};
    require(GetUnitProductionCostRule(0, &info, sizeof(info)) ==
                    UNIT_PRODUCTION_COST_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.percentageDelta == 50,
            "production cost readback ordering mismatch");

    const auto evaluation = evaluate();
    require(evaluation.nativePercent == 100 &&
                evaluation.finalPercent == 125 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "production cost additive composition mismatch");

    const UnitProductionCostQuery impiQuery = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_ZULU,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_IMPI_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation impi{};
    require(EvaluateUnitProductionCost(
                &impiQuery, &impi, sizeof(impi)) ==
                    UNIT_PRODUCTION_COST_RULES_OK &&
                impi.finalPercent == 100 && impi.additiveCount == 0,
            "production cost identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto overflow = makeRule(
        "a.provider", "overflow", std::numeric_limits<int32_t>::max());
    require(RegisterUnitProductionCostRule(&overflow) ==
                UNIT_PRODUCTION_COST_RULES_OK,
            "production cost overflow rule did not register");
    const auto evaluation = evaluate();
    require(evaluation.finalPercent == evaluation.nativePercent &&
                (evaluation.statusFlags &
                 UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0,
            "production cost overflow did not preserve native percent");

    rerevved::unit_production_cost_rules::ResetForTests();
    auto nonpositive = makeRule("a.provider", "nonpositive", -100);
    require(RegisterUnitProductionCostRule(&nonpositive) ==
                UNIT_PRODUCTION_COST_RULES_OK,
            "production cost nonpositive rule did not register");
    const auto invalid = evaluate();
    require(invalid.finalPercent == invalid.nativePercent &&
                (invalid.statusFlags &
                 UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0,
            "nonpositive production cost did not preserve native percent");

    const UnitProductionCostQuery query = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation output{};
    require(EvaluateUnitProductionCost(&query, &output, 19) ==
                UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL,
            "short production cost evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(EvaluateUnitProductionCost(&query, &output, 20) ==
                    UNIT_PRODUCTION_COST_RULES_OK &&
                output.structSize ==
                    sizeof(output),
            "production cost minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        require(bytes[index] == 0x5a,
                "production cost minimum prefix overwrote caller tail");
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
