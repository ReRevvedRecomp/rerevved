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

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedUnitProductionCostRule MakeRule(const char* provider,
                                        const char* rule_id,
                                        int32_t     percentage_delta)
{
    ReRevvedUnitProductionCostRule rule{};
    rule.struct_size = sizeof(rule);
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    rule.civilization     = REREVVED_CIVILIZATION_AZTEC;
    rule.base_unit_type   = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity         = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.percentage_delta = percentage_delta;
    return rule;
}

ReRevvedUnitProductionCostEvaluation Evaluate()
{
    const ReRevvedUnitProductionCostQuery query = {
        sizeof(ReRevvedUnitProductionCostQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    ReRevvedUnitProductionCostEvaluation evaluation{};
    Require(ReRevvedEvaluateUnitProductionCost(
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
                           percentage_delta) == 144);
    static_assert(sizeof(ReRevvedUnitProductionCostRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitProductionCostRuleInfo, status_flags) == 148);
    static_assert(sizeof(ReRevvedUnitProductionCostQuery) == 40);
    static_assert(sizeof(ReRevvedUnitProductionCostEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitProductionCostEvaluation, final_percent) == 8);
    Require(ReRevvedUnitProductionCostRulesAbiVersion() ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION,
            "production cost ABI version mismatch");

    rerevved::unit_production_cost_rules::ResetForTests();
    Require(ReRevvedRegisterUnitProductionCostRule(nullptr) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "null production cost rule accepted");
    auto invalid     = MakeRule("test.provider", "invalid", 1);
    invalid.identity = REREVVED_UNIT_IDENTITY_BASE;
    Require(ReRevvedRegisterUnitProductionCostRule(&invalid) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "base identity production cost rule accepted");
    invalid             = MakeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    Require(ReRevvedRegisterUnitProductionCostRule(&invalid) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT,
            "nonzero production cost reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto later   = MakeRule("z.provider", "late", -25);
    auto earlier = MakeRule("a.provider", "early", 50);
    Require(ReRevvedRegisterUnitProductionCostRule(&later) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost rules did not register");
    Require(ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "idempotent production cost registration failed");
    earlier.percentage_delta = 40;
    Require(ReRevvedRegisterUnitProductionCostRule(&earlier) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting production cost rule id accepted");

    uint32_t count = 0;
    Require(ReRevvedGetUnitProductionCostRuleCount(&count) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                count == 2,
            "production cost rule count mismatch");
    ReRevvedUnitProductionCostRuleInfo info{};
    Require(ReRevvedGetUnitProductionCostRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.percentage_delta == 50,
            "production cost readback ordering mismatch");

    const auto evaluation = Evaluate();
    Require(evaluation.native_percent == 100 &&
                evaluation.final_percent == 125 &&
                evaluation.additive_count == 2 && evaluation.status_flags == 0,
            "production cost additive composition mismatch");

    const ReRevvedUnitProductionCostQuery impi_query = {
        sizeof(ReRevvedUnitProductionCostQuery),
        REREVVED_CIVILIZATION_ZULU,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
        {},
    };
    ReRevvedUnitProductionCostEvaluation impi{};
    Require(ReRevvedEvaluateUnitProductionCost(
                &impi_query, &impi, sizeof(impi)) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                impi.final_percent == 100 && impi.additive_count == 0,
            "production cost identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto overflow = MakeRule(
        "a.provider", "overflow", std::numeric_limits<int32_t>::max());
    Require(ReRevvedRegisterUnitProductionCostRule(&overflow) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost overflow rule did not register");
    const auto evaluation = Evaluate();
    Require(evaluation.final_percent == evaluation.native_percent &&
                (evaluation.status_flags &
                 REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0,
            "production cost overflow did not preserve native percent");

    rerevved::unit_production_cost_rules::ResetForTests();
    auto nonpositive = MakeRule("a.provider", "nonpositive", -100);
    Require(ReRevvedRegisterUnitProductionCostRule(&nonpositive) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_OK,
            "production cost nonpositive rule did not register");
    const auto invalid = Evaluate();
    Require(invalid.final_percent == invalid.native_percent &&
                (invalid.status_flags &
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
    Require(ReRevvedEvaluateUnitProductionCost(&query, &output, 19) ==
                REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL,
            "short production cost evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    Require(ReRevvedEvaluateUnitProductionCost(&query, &output, 20) ==
                    REREVVED_UNIT_PRODUCTION_COST_RULES_OK &&
                output.struct_size ==
                    sizeof(output),
            "production cost minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        Require(bytes[index] == 0x5a,
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
