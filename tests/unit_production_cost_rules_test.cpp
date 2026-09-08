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

ReRevvedUnitProductionCostRule makeRule(const char* provider,
                                        const char* ruleId,
                                        int32_t     percentageDelta)
{
    ReRevvedUnitProductionCostRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization    = REREVVED_CIVILIZATION_AZTEC;
    rule.baseUnitType    = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity        = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.percentageDelta = percentageDelta;
    return rule;
}

ReRevvedUnitProductionCostEvaluation evaluate()
{
    const ReRevvedUnitProductionCostQuery query = {
        sizeof(ReRevvedUnitProductionCostQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    ReRevvedUnitProductionCostEvaluation evaluation{};
    require(ReRevvedEvaluateUnitProductionCost(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(ReRevvedUnitProductionCostRule) == 168);
    static_assert(offsetof(ReRevvedUnitProductionCostRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUnitProductionCostRule,
                           percentageDelta) == 144);
    static_assert(sizeof(ReRevvedUnitProductionCostRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitProductionCostRuleInfo, statusFlags) == 148);
    static_assert(sizeof(ReRevvedUnitProductionCostQuery) == 40);
    static_assert(sizeof(ReRevvedUnitProductionCostEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitProductionCostEvaluation, finalPercent) == 8);
    require(ReRevvedUnitProductionCostRulesAbiVersion() ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION,
            "production cost ABI version mismatch");

    rerevved::unit_production_cost_rules::ResetForTests();
    require(ReRevvedRegisterUnitProductionCostRule(nullptr) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "null production cost rule accepted");
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = REREVVED_UNIT_IDENTITY_BASE;
    require(ReRevvedRegisterUnitProductionCostRule(&invalid) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "base identity production cost rule accepted");
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    require(ReRevvedRegisterUnitProductionCostRule(&invalid) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "nonzero production cost reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -25);
    auto earlier = makeRule("a.provider", "early", 50);
    require(ReRevvedRegisterUnitProductionCostRule(&later) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost rules did not register");
    require(ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "idempotent production cost registration failed");
    earlier.percentageDelta = 40;
    require(ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting production cost rule id accepted");

    uint32_t count = 0;
    require(ReRevvedGetUnitProductionCostRuleCount(&count) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                count == 2,
            "production cost rule count mismatch");
    ReRevvedUnitProductionCostRuleInfo info{};
    require(ReRevvedGetUnitProductionCostRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.percentageDelta == 50,
            "production cost readback ordering mismatch");

    const auto evaluation = evaluate();
    require(evaluation.nativePercent == 100 &&
                evaluation.finalPercent == 125 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "production cost additive composition mismatch");

    const ReRevvedUnitProductionCostQuery impiQuery = {
        sizeof(ReRevvedUnitProductionCostQuery),
        REREVVED_CIVILIZATION_ZULU,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
        {},
    };
    ReRevvedUnitProductionCostEvaluation impi{};
    require(ReRevvedEvaluateUnitProductionCost(
                &impiQuery, &impi, sizeof(impi)) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                impi.finalPercent == 100 && impi.additiveCount == 0,
            "production cost identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto overflow = makeRule(
        "a.provider", "overflow", std::numeric_limits<int32_t>::max());
    require(ReRevvedRegisterUnitProductionCostRule(&overflow) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost overflow rule did not register");
    const auto evaluation = evaluate();
    require(evaluation.finalPercent == evaluation.nativePercent &&
                (evaluation.statusFlags &
                 REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0,
            "production cost overflow did not preserve native percent");

    rerevved::unit_production_cost_rules::ResetForTests();
    auto nonpositive = makeRule("a.provider", "nonpositive", -100);
    require(ReRevvedRegisterUnitProductionCostRule(&nonpositive) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost nonpositive rule did not register");
    const auto invalid = evaluate();
    require(invalid.finalPercent == invalid.nativePercent &&
                (invalid.statusFlags &
                 REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0,
            "nonpositive production cost did not preserve native percent");

    const ReRevvedUnitProductionCostQuery query = {
        sizeof(ReRevvedUnitProductionCostQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    ReRevvedUnitProductionCostEvaluation output{};
    require(ReRevvedEvaluateUnitProductionCost(&query, &output, 19) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL,
            "short production cost evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(ReRevvedEvaluateUnitProductionCost(&query, &output, 20) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
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
