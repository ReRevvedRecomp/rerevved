#include <unit_production_cost_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitProductionCostRulesAbiVersionFn versionFn =
        ReRevvedUnitProductionCostRulesAbiVersion;
    ReRevvedRegisterUnitProductionCostRuleFn registerFn =
        ReRevvedRegisterUnitProductionCostRule;
    ReRevvedGetUnitProductionCostRuleCountFn countFn =
        ReRevvedGetUnitProductionCostRuleCount;
    ReRevvedGetUnitProductionCostRuleFn  getFn      = ReRevvedGetUnitProductionCostRule;
    ReRevvedEvaluateUnitProductionCostFn evaluateFn = ReRevvedEvaluateUnitProductionCost;

    ReRevvedUnitProductionCostRule       rule       = { 0 };
    ReRevvedUnitProductionCostRuleInfo   info       = { 0 };
    ReRevvedUnitProductionCostQuery      query      = { 0 };
    ReRevvedUnitProductionCostEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitProductionCostRule, civilization) != 132 ||
        offsetof(ReRevvedUnitProductionCostRule, percentageDelta) != 144 ||
        offsetof(ReRevvedUnitProductionCostRuleInfo, statusFlags) != 148 ||
        offsetof(ReRevvedUnitProductionCostQuery, reserved) != 16 ||
        offsetof(ReRevvedUnitProductionCostEvaluation, finalPercent) != 8 ||
        REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION != 1u)
    {
        return 1;
    }
    return versionFn() == REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION ? 0 : 2;
}
