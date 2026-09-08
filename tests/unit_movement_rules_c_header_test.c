#include <unit_movement_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitMovementRulesAbiVersionFn versionFn =
        ReRevvedUnitMovementRulesAbiVersion;
    ReRevvedRegisterUnitMovementRuleFn registerFn =
        ReRevvedRegisterUnitMovementRule;
    ReRevvedGetUnitMovementRuleCountFn countFn =
        ReRevvedGetUnitMovementRuleCount;
    ReRevvedGetUnitMovementRuleFn  getFn      = ReRevvedGetUnitMovementRule;
    ReRevvedEvaluateUnitMovementFn evaluateFn = ReRevvedEvaluateUnitMovement;

    ReRevvedUnitMovementRule       rule       = { 0 };
    ReRevvedUnitMovementRuleInfo   info       = { 0 };
    ReRevvedUnitMovementQuery      query      = { 0 };
    ReRevvedUnitMovementEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitMovementRule, civilization) != 132 ||
        offsetof(ReRevvedUnitMovementRule, value) != 144 ||
        offsetof(ReRevvedUnitMovementRuleInfo, statusFlags) != 148 ||
        offsetof(ReRevvedUnitMovementQuery, nativeValue) != 16 ||
        offsetof(ReRevvedUnitMovementEvaluation, finalValue) != 8 ||
        REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION != 1u)
    {
        return 1;
    }
    return versionFn() == REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION ? 0 : 2;
}
