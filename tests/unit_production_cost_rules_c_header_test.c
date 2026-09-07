#include <unit_production_cost_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitProductionCostRulesAbiVersionFn version_fn =
        ReRevvedUnitProductionCostRulesAbiVersion;
    ReRevvedRegisterUnitProductionCostRuleFn register_fn =
        ReRevvedRegisterUnitProductionCostRule;
    ReRevvedGetUnitProductionCostRuleCountFn count_fn =
        ReRevvedGetUnitProductionCostRuleCount;
    ReRevvedGetUnitProductionCostRuleFn get_fn       = ReRevvedGetUnitProductionCostRule;
    ReRevvedEvaluateUnitProductionCostFn evaluate_fn = ReRevvedEvaluateUnitProductionCost;

    ReRevvedUnitProductionCostRule rule             = { 0 };
    ReRevvedUnitProductionCostRuleInfo info         = { 0 };
    ReRevvedUnitProductionCostQuery query           = { 0 };
    ReRevvedUnitProductionCostEvaluation evaluation = { 0 };

    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitProductionCostRule, civilization) != 132 ||
        offsetof(ReRevvedUnitProductionCostRule, percentage_delta) != 144 ||
        offsetof(ReRevvedUnitProductionCostRuleInfo, status_flags) != 148 ||
        offsetof(ReRevvedUnitProductionCostQuery, reserved) != 16 ||
        offsetof(ReRevvedUnitProductionCostEvaluation, final_percent) != 8 ||
        REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION != 1u)
    {
        return 1;
    }
    return version_fn() == REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION ? 0 : 2;
}
