#include <unique_era_abilities.h>

#include <stddef.h>

int main(void)
{
    if (ERA_ABILITIES_ABI_VERSION != 3u ||
        ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING != 0x10000)
    {
        return 1;
    }
    EraAbilitiesAbiVersionFn versionFn =
        EraAbilitiesAbiVersion;
    RegisterEraAbilityReplacementFn registerFn =
        RegisterEraAbilityReplacement;
    GetEraAbilityRuleCountFn countFn =
        GetEraAbilityRuleCount;
    GetEraAbilityRuleFn getFn =
        GetEraAbilityRule;
    EvaluateEraAbilityCellFn evaluateFn =
        EvaluateEraAbilityCell;

    EraAbilityReplacement    rule       = { 0 };
    EraAbilityRuleInfo       info       = { 0 };
    EraAbilityCellQuery      query      = { 0 };
    EraAbilityCellEvaluation evaluation = { 0 };

    if (versionFn() != ERA_ABILITIES_ABI_VERSION ||
        sizeof(rule) != 176 || sizeof(info) != 180 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(EraAbilityReplacement, replacementAbility) !=
            140 ||
        offsetof(EraAbilityRuleInfo, statusFlags) != 144 ||
        offsetof(EraAbilityCellEvaluation, effectiveAbility) !=
            8)
    {
        return 1;
    }

    (void)registerFn;
    (void)countFn;
    (void)getFn;
    (void)evaluateFn;
    return 0;
}
