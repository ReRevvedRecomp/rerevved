#include <unit_movement_rules.h>

#include <stddef.h>

int main(void)
{
    UnitMovementRulesAbiVersionFn versionFn =
        UnitMovementRulesAbiVersion;
    RegisterUnitMovementRuleFn registerFn =
        RegisterUnitMovementRule;
    GetUnitMovementRuleCountFn countFn =
        GetUnitMovementRuleCount;
    GetUnitMovementRuleFn  getFn      = GetUnitMovementRule;
    EvaluateUnitMovementFn evaluateFn = EvaluateUnitMovement;

    UnitMovementRule       rule       = { 0 };
    UnitMovementRuleInfo   info       = { 0 };
    UnitMovementQuery      query      = { 0 };
    UnitMovementEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(UnitMovementRule, civilization) != 132 ||
        offsetof(UnitMovementRule, value) != 144 ||
        offsetof(UnitMovementRuleInfo, statusFlags) != 148 ||
        offsetof(UnitMovementQuery, nativeValue) != 16 ||
        offsetof(UnitMovementEvaluation, finalValue) != 8 ||
        UNIT_MOVEMENT_RULES_ABI_VERSION != 2u)
    {
        return 1;
    }
    return versionFn() == UNIT_MOVEMENT_RULES_ABI_VERSION ? 0 : 2;
}
