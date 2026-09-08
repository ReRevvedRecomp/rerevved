#include <unit_movement_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitMovementRulesAbiVersionFn version_fn =
        ReRevvedUnitMovementRulesAbiVersion;
    ReRevvedRegisterUnitMovementRuleFn register_fn =
        ReRevvedRegisterUnitMovementRule;
    ReRevvedGetUnitMovementRuleCountFn count_fn =
        ReRevvedGetUnitMovementRuleCount;
    ReRevvedGetUnitMovementRuleFn  get_fn      = ReRevvedGetUnitMovementRule;
    ReRevvedEvaluateUnitMovementFn evaluate_fn = ReRevvedEvaluateUnitMovement;

    ReRevvedUnitMovementRule       rule       = { 0 };
    ReRevvedUnitMovementRuleInfo   info       = { 0 };
    ReRevvedUnitMovementQuery      query      = { 0 };
    ReRevvedUnitMovementEvaluation evaluation = { 0 };

    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitMovementRule, civilization) != 132 ||
        offsetof(ReRevvedUnitMovementRule, value) != 144 ||
        offsetof(ReRevvedUnitMovementRuleInfo, status_flags) != 148 ||
        offsetof(ReRevvedUnitMovementQuery, native_value) != 16 ||
        offsetof(ReRevvedUnitMovementEvaluation, final_value) != 8 ||
        REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION != 1u)
    {
        return 1;
    }
    return version_fn() == REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION ? 0 : 2;
}
