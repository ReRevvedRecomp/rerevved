#include <unit_production_cost_rules.h>

#include <stddef.h>

int main(void)
{
    UnitProductionCostRulesAbiVersionFn versionFn =
        UnitProductionCostRulesAbiVersion;
    RegisterUnitProductionCostRuleFn registerFn =
        RegisterUnitProductionCostRule;
    GetUnitProductionCostRuleCountFn countFn =
        GetUnitProductionCostRuleCount;
    GetUnitProductionCostRuleFn  getFn      = GetUnitProductionCostRule;
    EvaluateUnitProductionCostFn evaluateFn = EvaluateUnitProductionCost;

    UnitProductionCostRule       rule       = { 0 };
    UnitProductionCostRuleInfo   info       = { 0 };
    UnitProductionCostQuery      query      = { 0 };
    UnitProductionCostEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(UnitProductionCostRule, civilization) != 132 ||
        offsetof(UnitProductionCostRule, percentageDelta) != 144 ||
        offsetof(UnitProductionCostRuleInfo, statusFlags) != 148 ||
        offsetof(UnitProductionCostQuery, reserved) != 16 ||
        offsetof(UnitProductionCostEvaluation, finalPercent) != 8 ||
        UNIT_PRODUCTION_COST_RULES_ABI_VERSION != 2u)
    {
        return 1;
    }
    return versionFn() == UNIT_PRODUCTION_COST_RULES_ABI_VERSION ? 0 : 2;
}
